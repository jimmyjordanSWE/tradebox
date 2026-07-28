#include "tradebox/core/trading_core.h"

#include <algorithm>
#include <array>
#include <type_traits>
#include <utility>

namespace tradebox::core {

TradingCore::TradingCore(IEventJournal& journal, IClock& clock)
    : journal_(journal), clock_(clock) {}

CoreSnapshot TradingCore::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return state_;
}

std::optional<CoreSnapshot> TradingCore::SnapshotAfter(
    std::uint64_t revision) const {
    std::scoped_lock lock(mutex_);
    if (state_.revision == revision) return std::nullopt;
    return state_;
}

std::expected<CommandReceipt, CoreError> TradingCore::Submit(Command command) {
    std::scoped_lock lock(mutex_);
    CommandReceipt receipt{
        .command_id = next_command_id_++,
        .accepted = true,
    };

    std::visit(
        [this, &receipt](const auto& typed_command) {
            using T = std::decay_t<decltype(typed_command)>;
            if constexpr (std::is_same_v<T, ConnectAccount>) {
                state_.environment = typed_command.environment;
                ClearFinancialState();
                ResetConnectionState(SafetyStatus::Connecting,
                                     "Connection requested");
                receipt.message = "Connection accepted";
            } else if constexpr (std::is_same_v<T, DisconnectAccount>) {
                ClearFinancialState();
                ResetConnectionState(SafetyStatus::Disconnected,
                                     "Disconnected");
                receipt.message = "Disconnect accepted";
            } else if constexpr (
                std::is_same_v<T, RequestReconciliation>) {
                if (state_.generation.value == 0) {
                    receipt.accepted = false;
                    receipt.message =
                        "Cannot reconcile before a connection generation exists";
                    return;
                }
                state_.reconciled = false;
                state_.safety_status = SafetyStatus::Reconciling;
                state_.status_message = "Reconciliation requested";
                ++state_.revision;
                receipt.message = "Reconciliation accepted";
            }
        },
        command);

    return receipt;
}

std::expected<void, CoreError> TradingCore::Ingest(BrokerEvent event) {
    if (event.received_at == std::chrono::system_clock::time_point{})
        event.received_at = clock_.Now();

    std::scoped_lock lock(mutex_);
    if (auto appended = journal_.Append(event); !appended)
        return std::unexpected(CoreError{
            .code = CoreErrorCode::JournalFailure,
            .message = appended.error().message,
        });

    if (event.kind != BrokerEventKind::ConnectionAttemptStarted &&
        event.generation < state_.generation) {
        return std::unexpected(CoreError{
            .code = CoreErrorCode::StaleGeneration,
            .message = "Broker event belongs to an older connection generation",
        });
    }

    switch (event.kind) {
        case BrokerEventKind::ConnectionAttemptStarted:
            if (event.generation <= state_.generation) {
                return std::unexpected(CoreError{
                    .code = CoreErrorCode::StaleGeneration,
                    .message =
                        "Connection generation must increase monotonically",
                });
            }
            state_.generation = event.generation;
            ClearFinancialState();
            ResetConnectionState(SafetyStatus::Connecting,
                                 "Connecting to broker");
            break;
        case BrokerEventKind::Authorized:
            state_.authenticated = true;
            state_.safety_status = SafetyStatus::SnapshotLoading;
            state_.status_message = "Broker authorized; loading snapshot";
            ++state_.revision;
            break;
        case BrokerEventKind::TradeUpdatesAcknowledged:
            state_.trade_updates_acknowledged = true;
            ++state_.revision;
            RefreshSafetyStatus();
            break;
        case BrokerEventKind::AccountSnapshot: {
            const auto* snapshot =
                std::get_if<AccountSnapshotPayload>(&event.payload);
            if (!snapshot) {
                return std::unexpected(CoreError{
                    .code = CoreErrorCode::InvalidTransition,
                    .message = "Account snapshot payload is missing",
                });
            }
            state_.account = snapshot->account;
            state_.account_snapshot_loaded = true;
            state_.trading_permitted =
                snapshot->account.status == "ACTIVE" &&
                !snapshot->account.account_blocked &&
                !snapshot->account.trade_suspended_by_user &&
                !snapshot->account.trading_blocked;
            if (state_.positions_snapshot_loaded &&
                state_.orders_snapshot_loaded)
                state_.reconciled = true;
            ++state_.revision;
            RefreshSafetyStatus();
            break;
        }
        case BrokerEventKind::PositionsSnapshot: {
            const auto* snapshot =
                std::get_if<PositionsSnapshotPayload>(&event.payload);
            if (!snapshot) {
                return std::unexpected(CoreError{
                    .code = CoreErrorCode::InvalidTransition,
                    .message = "Positions snapshot payload is missing",
                });
            }
            std::unordered_map<std::string, PositionState> replacement;
            replacement.reserve(snapshot->positions.size());
            for (PositionState position : snapshot->positions) {
                if (position.asset_id.empty()) {
                    return std::unexpected(CoreError{
                        .code = CoreErrorCode::InvalidTransition,
                        .message =
                            "Position snapshot contains a position without an asset ID",
                    });
                }
                position.provisional = false;
                replacement.insert_or_assign(position.asset_id,
                                             std::move(position));
            }
            positions_by_asset_id_ = std::move(replacement);
            state_.positions_snapshot_loaded = true;
            state_.positions_received_at_ms = snapshot->received_at_ms;
            if (state_.account_snapshot_loaded &&
                state_.orders_snapshot_loaded)
                state_.reconciled = true;
            PublishPositions();
            ++state_.revision;
            RefreshSafetyStatus();
            break;
        }
        case BrokerEventKind::OrdersSnapshot: {
            const auto* snapshot =
                std::get_if<OrdersSnapshotPayload>(&event.payload);
            if (!snapshot) {
                return std::unexpected(CoreError{
                    .code = CoreErrorCode::InvalidTransition,
                    .message = "Orders snapshot payload is missing",
                });
            }
            if (auto applied = ApplyOrdersSnapshot(*snapshot); !applied)
                return applied;
            state_.orders_snapshot_loaded = true;
            state_.orders_received_at_ms = snapshot->received_at_ms;
            for (const TradeUpdatePayload& buffered :
                 buffered_trade_updates_) {
                if (auto applied = ApplyTradeUpdate(buffered); !applied)
                    return applied;
            }
            buffered_trade_updates_.clear();
            state_.reconciled = state_.account_snapshot_loaded &&
                                state_.positions_snapshot_loaded;
            ++state_.revision;
            PublishOrders();
            RefreshSafetyStatus();
            break;
        }
        case BrokerEventKind::TradeUpdate: {
            const auto* update =
                std::get_if<TradeUpdatePayload>(&event.payload);
            if (!update) {
                return std::unexpected(CoreError{
                    .code = CoreErrorCode::InvalidTransition,
                    .message = "Trade update payload is missing",
                });
            }
            if (!state_.orders_snapshot_loaded) {
                buffered_trade_updates_.push_back(*update);
                state_.status_message =
                    "Trade update buffered until order snapshot arrives";
                ++state_.revision;
                break;
            }
            if (auto applied = ApplyTradeUpdate(*update); !applied)
                return applied;
            state_.last_trade_update_at_ms = update->event_at_ms;
            ++state_.revision;
            PublishOrders();
            RefreshSafetyStatus();
            break;
        }
        case BrokerEventKind::ReconciliationStarted:
            reconciliation_required_ = true;
            state_.reconciled = false;
            state_.safety_status = SafetyStatus::Reconciling;
            state_.status_message = "Reconciling broker state";
            ++state_.revision;
            break;
        case BrokerEventKind::ReconciliationCompleted:
            reconciliation_required_ = false;
            state_.reconciled = true;
            ++state_.revision;
            RefreshSafetyStatus();
            break;
        case BrokerEventKind::Disconnected:
            ResetConnectionState(SafetyStatus::Stale,
                                 "Trading stream disconnected");
            break;
        case BrokerEventKind::Failure:
            state_.safety_status = SafetyStatus::Error;
            state_.status_message =
                event.message.empty() ? "Broker failure" : event.message;
            ++state_.revision;
            break;
    }

    return {};
}

void TradingCore::RefreshSafetyStatus() {
    state_.initial_snapshot_loaded =
        state_.account_snapshot_loaded && state_.positions_snapshot_loaded &&
        state_.orders_snapshot_loaded;

    if (state_.authenticated && state_.trade_updates_acknowledged &&
        state_.initial_snapshot_loaded && state_.reconciled) {
        reconciliation_required_ = false;
        state_.safety_status = SafetyStatus::Live;
        state_.status_message = "Broker state live and reconciled";
    } else if (reconciliation_required_) {
        state_.safety_status = SafetyStatus::Reconciling;
        state_.status_message =
            "Broker state changed; REST reconciliation pending";
    } else if (state_.initial_snapshot_loaded) {
        state_.safety_status = SafetyStatus::Reconciling;
        state_.status_message = "Snapshot loaded; reconciliation pending";
    } else {
        state_.safety_status = SafetyStatus::SnapshotLoading;
        state_.status_message = "Initial broker snapshot pending";
    }
}

void TradingCore::ResetConnectionState(SafetyStatus status,
                                       std::string message) {
    reconciliation_required_ = false;
    state_.authenticated = false;
    state_.trade_updates_acknowledged = false;
    state_.account_snapshot_loaded = false;
    state_.positions_snapshot_loaded = false;
    state_.orders_snapshot_loaded = false;
    state_.initial_snapshot_loaded = false;
    state_.reconciled = false;
    state_.trading_permitted = false;
    state_.safety_status = status;
    state_.status_message = std::move(message);
    ++state_.revision;
}

std::expected<void, CoreError> TradingCore::ApplyOrdersSnapshot(
    const OrdersSnapshotPayload& snapshot) {
    std::unordered_map<std::string, OrderState> replacement;
    replacement.reserve(snapshot.orders.size());
    for (const OrderState& order : snapshot.orders) {
        if (order.id.empty()) {
            return std::unexpected(CoreError{
                .code = CoreErrorCode::InvalidTransition,
                .message = "Order snapshot contains an order without an ID",
            });
        }
        replacement.insert_or_assign(order.id, order);
    }
    orders_by_id_ = std::move(replacement);
    return {};
}

std::expected<void, CoreError> TradingCore::ApplyTradeUpdate(
    const TradeUpdatePayload& update) {
    static constexpr std::array<std::string_view, 20> known_events = {
        "new",          "accepted",       "pending_new",
        "partial_fill", "fill",           "pending_cancel",
        "canceled",     "pending_replace", "replaced",
        "expired",      "done_for_day",   "rejected",
        "stopped",      "suspended",      "calculated",
        "held",         "order_cancel_rejected",
        "order_replace_rejected", "trade_bust", "trade_correct",
    };
    if (std::find(known_events.begin(), known_events.end(), update.event) ==
        known_events.end()) {
        state_.safety_status = SafetyStatus::Error;
        state_.status_message = "Unhandled trade update: " + update.event;
        ++state_.revision;
        return std::unexpected(CoreError{
            .code = CoreErrorCode::InvalidTransition,
            .message = state_.status_message,
        });
    }
    if (update.order.id.empty()) {
        return std::unexpected(CoreError{
            .code = CoreErrorCode::InvalidTransition,
            .message = "Trade update order ID is empty",
        });
    }

    const bool execution_event =
        update.event == "fill" || update.event == "partial_fill";
    if (execution_event && !update.execution_id.empty() &&
        execution_ids_.contains(update.execution_id)) {
        return {};
    }

    const auto found = orders_by_id_.find(update.order.id);
    if (found != orders_by_id_.end()) {
        const OrderState& current = found->second;
        if (update.order.updated_at_ms > 0 && current.updated_at_ms > 0 &&
            update.order.updated_at_ms < current.updated_at_ms) {
            return {};
        }
        if (execution_event &&
            update.order.filled_qty < current.filled_qty) {
            state_.safety_status = SafetyStatus::Reconciling;
            state_.reconciled = false;
            state_.status_message =
                "Filled quantity regressed; reconciliation required";
            return std::unexpected(CoreError{
                .code = CoreErrorCode::InvalidTransition,
                .message = state_.status_message,
            });
        }
    }
    if (update.order.qty &&
        update.order.filled_qty > *update.order.qty) {
        state_.safety_status = SafetyStatus::Reconciling;
        state_.reconciled = false;
        state_.status_message =
            "Filled quantity exceeds order quantity; reconciliation required";
        return std::unexpected(CoreError{
            .code = CoreErrorCode::InvalidTransition,
            .message = state_.status_message,
        });
    }

    OrderState accepted = update.order;
    accepted.last_event = update.event;
    orders_by_id_.insert_or_assign(accepted.id, std::move(accepted));
    if (execution_event && !update.execution_id.empty())
        execution_ids_.insert(update.execution_id);
    if (update.event == "trade_bust" || update.event == "trade_correct") {
        state_.reconciled = false;
        state_.safety_status = SafetyStatus::Reconciling;
        state_.status_message =
            "Execution correction received; REST reconciliation required";
    }
    if (execution_event || update.event == "trade_bust" ||
        update.event == "trade_correct") {
        reconciliation_required_ = true;
        state_.account_snapshot_loaded = false;
        state_.positions_snapshot_loaded = false;
        state_.reconciled = false;
        if (!update.order.asset_id.empty() && update.position_qty) {
            PositionState& position =
                positions_by_asset_id_[update.order.asset_id];
            position.asset_id = update.order.asset_id;
            position.symbol = update.order.symbol;
            position.asset_class = update.order.asset_class;
            position.qty = *update.position_qty;
            position.provisional = true;
            PublishPositions();
        }
    }
    return {};
}

void TradingCore::PublishOrders() {
    state_.orders.clear();
    state_.orders.reserve(orders_by_id_.size());
    for (const auto& [id, order] : orders_by_id_) {
        static_cast<void>(id);
        state_.orders.push_back(order);
    }
    std::ranges::sort(
        state_.orders,
        [](const OrderState& left, const OrderState& right) {
            if (left.submitted_at_ms != right.submitted_at_ms)
                return left.submitted_at_ms > right.submitted_at_ms;
            return left.id < right.id;
        });
}

void TradingCore::PublishPositions() {
    state_.positions.clear();
    state_.positions.reserve(positions_by_asset_id_.size());
    for (const auto& [asset_id, position] : positions_by_asset_id_) {
        static_cast<void>(asset_id);
        state_.positions.push_back(position);
    }
    std::ranges::sort(
        state_.positions,
        [](const PositionState& left, const PositionState& right) {
            if (left.symbol != right.symbol)
                return left.symbol < right.symbol;
            return left.asset_id < right.asset_id;
        });
}

void TradingCore::ClearFinancialState() {
    state_.account.reset();
    state_.positions.clear();
    state_.orders.clear();
    state_.positions_received_at_ms = 0;
    state_.orders_received_at_ms = 0;
    state_.last_trade_update_at_ms = 0;
    orders_by_id_.clear();
    positions_by_asset_id_.clear();
    execution_ids_.clear();
    buffered_trade_updates_.clear();
}

}  // namespace tradebox::core
