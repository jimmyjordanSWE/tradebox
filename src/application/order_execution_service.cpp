#include "tradebox/application/order_execution_service.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>
#include <thread>
#include <type_traits>
#include <utility>

namespace tradebox::application {
namespace {

const core::OrderCommandContext& Context(
    const core::NativeOrderCommand& command) {
    return std::visit(
        [](const auto& typed) -> const core::OrderCommandContext& {
            return typed.context;
        },
        command);
}

core::OrderCommandKind Kind(const core::NativeOrderCommand& command) {
    return std::visit(
        [](const auto& typed) {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, core::PlaceOrderCommand>) {
                return core::OrderCommandKind::Place;
            } else if constexpr (
                std::is_same_v<T, core::CancelOrderCommand>) {
                return core::OrderCommandKind::Cancel;
            } else if constexpr (
                std::is_same_v<T, core::ReplaceOrderCommand>) {
                return core::OrderCommandKind::Replace;
            } else if constexpr (
                std::is_same_v<T, core::AmendBracketOrderCommand>) {
                return core::OrderCommandKind::AmendBracket;
            } else if constexpr (
                std::is_same_v<T, core::ClosePositionCommand>) {
                return core::OrderCommandKind::ClosePosition;
            } else if constexpr (
                std::is_same_v<T, core::CloseAllPositionsCommand>) {
                return core::OrderCommandKind::CloseAllPositions;
            } else {
                return core::OrderCommandKind::CancelAllOrders;
            }
        },
        command);
}

std::string RequestId(const core::NativeOrderCommand& command) {
    return Context(command).request_id;
}

std::string DerivedClientOrderId(std::string_view request_id) {
    std::string result = "tb-";
    result.reserve(
        std::min<std::size_t>(128, result.size() + request_id.size()));
    for (unsigned char character : request_id) {
        if (result.size() == 128) break;
        if (std::isalnum(character) || character == '-' ||
            character == '_')
            result.push_back(static_cast<char>(character));
        else
            result.push_back('-');
    }
    return result;
}

std::string ValidationMessage(
    const std::vector<core::OrderValidationError>& errors) {
    std::ostringstream message;
    for (std::size_t index = 0; index < errors.size(); ++index) {
        if (index != 0) message << "; ";
        message << errors[index].field << ": " << errors[index].message;
    }
    return message.str();
}

core::OrderCommandResult Rejected(
    std::string request_id, core::OrderCommandOutcome outcome,
    std::string message) {
    return {
        .request_id = std::move(request_id),
        .outcome = outcome,
        .message = std::move(message),
    };
}

bool RequiresLiveSafety(const core::NativeOrderCommand& command) {
    return !std::holds_alternative<core::CancelOrderCommand>(command) &&
           !std::holds_alternative<core::CancelAllOrdersCommand>(command);
}

core::Decimal Absolute(const core::Decimal& value) {
    std::string text = value.ToString();
    if (!text.empty() && text.front() == '-') text.erase(text.begin());
    return *core::Decimal::Parse(text);
}

std::int64_t ToMilliseconds(
    std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               value.time_since_epoch())
        .count();
}

bool TerminalOrderStatus(std::string_view status) {
    return status == "filled" || status == "canceled" ||
           status == "cancelled" || status == "expired" ||
           status == "rejected" || status == "done_for_day" ||
           status == "stopped";
}

bool CanceledOrderStatus(std::string_view status) {
    return status == "canceled" || status == "cancelled";
}

}  // namespace

OrderExecutionService::OrderExecutionService(
    core::ITradingCore& core, broker::IOrderGateway& gateway,
    core::IOrderCommandJournal& journal, core::IClock& clock,
    const core::IMarketDataView* market_data)
    : core_(core),
      gateway_(gateway),
      journal_(journal),
      clock_(clock),
      market_data_(market_data) {
    const auto recoverable = journal_.Recoverable();
    recovery_pending_ = recoverable && !recoverable->empty();
    worker_ =
        std::thread(&OrderExecutionService::WorkerLoop, this);
}

OrderExecutionService::~OrderExecutionService() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::future<core::OrderCommandResult> OrderExecutionService::Submit(
    core::NativeOrderCommand command) {
    auto pending = std::make_unique<PendingCommand>();
    pending->command = std::move(command);
    std::future<core::OrderCommandResult> result =
        pending->completion.get_future();
    if (core_.Snapshot().safety_status ==
        core::SafetyStatus::Disconnected) {
        pending->completion.set_value(Rejected(
            RequestId(pending->command),
            core::OrderCommandOutcome::SafetyRejected,
            "Order was not sent because the broker session is "
            "disconnected"));
        return result;
    }
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) {
            pending->completion.set_value(Rejected(
                RequestId(pending->command),
                core::OrderCommandOutcome::ServiceStopped,
                "Order command service is stopped"));
            return result;
        }
        pending_.push(std::move(pending));
    }
    ready_.notify_one();
    return result;
}

std::expected<core::OrderCommandLookup, std::string>
OrderExecutionService::Lookup(const std::string& request_id) {
    if (request_id.empty())
        return std::unexpected("request_id is required");
    return journal_.Lookup(request_id);
}

void OrderExecutionService::WorkerLoop() {
    for (;;) {
        std::unique_ptr<PendingCommand> command;
        {
            std::unique_lock lock(mutex_);
            if (recovery_pending_)
                ready_.wait_for(
                    lock, std::chrono::milliseconds(250),
                    [this] {
                        return stopping_ || !pending_.empty();
                    });
            else
                ready_.wait(
                    lock,
                    [this] {
                        return stopping_ || !pending_.empty();
                    });
            if (pending_.empty() && stopping_) return;
            if (!pending_.empty()) {
                command = std::move(pending_.front());
                pending_.pop();
            }
        }
        if (command) {
            core::OrderCommandResult result =
                Execute(std::move(command->command));
            if (result.recovery_state ==
                core::CommandRecoveryState::Pending)
                recovery_pending_ = true;
            command->completion.set_value(std::move(result));
        }
        RecoverCommands();
    }
}

void OrderExecutionService::RecoverCommands() {
    if (!recovery_pending_) return;
    const auto recoverable = journal_.Recoverable();
    if (!recoverable) return;
    if (recoverable->empty()) {
        recovery_pending_ = false;
        return;
    }
    const core::CoreSnapshot snapshot = core_.Snapshot();
    for (const auto& command : *recoverable) {
        const auto resolution = ResolveRecovery(command, snapshot);
        if (!resolution) continue;
        static_cast<void>(journal_.ResolveRecovery(*resolution));
    }
}

std::optional<core::OrderCommandResult>
OrderExecutionService::ResolveRecovery(
    const core::RecoverableOrderCommand& command,
    const core::CoreSnapshot& snapshot) const {
    const auto result = [&command](
                            core::OrderCommandOutcome outcome,
                            core::CommandRecoveryState recovery_state,
                            std::string message,
                            std::string broker_order_id = {}) {
        return core::OrderCommandResult{
            .request_id = command.record.request_id,
            .outcome = outcome,
            .broker_order_id = std::move(broker_order_id),
            .message = message,
            .reconciliation_required = false,
            .recovery_state = recovery_state,
            .recovery_message = std::move(message),
        };
    };

    if (!command.dispatch_started)
        return result(
            core::OrderCommandOutcome::NotDispatched,
            core::CommandRecoveryState::Rejected,
            "Recovered reservation ended before broker dispatch began");

    if (!snapshot.account || !snapshot.initial_snapshot_loaded ||
        !snapshot.reconciled)
        return std::nullopt;
    if (snapshot.account->id != command.record.account_id ||
        snapshot.environment != command.record.environment)
        return result(
            core::OrderCommandOutcome::Indeterminate,
            core::CommandRecoveryState::OperatorAttention,
            "Recoverable command belongs to a different account or "
            "paper/live environment");

    constexpr std::int64_t grace_ms = 30'000;
    const bool grace_elapsed =
        ToMilliseconds(clock_.Now()) - command.record.created_at_ms >=
        grace_ms;
    const auto find_order_by_id = [&snapshot](std::string_view id) {
        return std::ranges::find_if(
            snapshot.orders, [id](const core::OrderState& order) {
                return order.id == id;
            });
    };
    const auto find_order_by_client_id =
        [&snapshot](std::string_view client_order_id) {
            return std::ranges::find_if(
                snapshot.orders,
                [client_order_id](const core::OrderState& order) {
                    return order.client_order_id == client_order_id;
                });
        };

    switch (command.record.kind) {
    case core::OrderCommandKind::Place: {
        if (command.record.client_order_id.empty())
            return result(
                core::OrderCommandOutcome::Indeterminate,
                core::CommandRecoveryState::OperatorAttention,
                "Placed order has no durable client_order_id");
        const auto order =
            find_order_by_client_id(command.record.client_order_id);
        if (order != snapshot.orders.end())
            return result(
                core::OrderCommandOutcome::BrokerAccepted,
                core::CommandRecoveryState::Resolved,
                "Recovered placed order by client_order_id",
                order->id);
        if (!grace_elapsed) return std::nullopt;
        return result(
            core::OrderCommandOutcome::RecoveryRejected,
            core::CommandRecoveryState::Rejected,
            "No authoritative broker order matches client_order_id");
    }
    case core::OrderCommandKind::Replace: {
        if (!command.record.client_order_id.empty()) {
            const auto replacement =
                find_order_by_client_id(
                    command.record.client_order_id);
            if (replacement != snapshot.orders.end())
                return result(
                    core::OrderCommandOutcome::BrokerAccepted,
                    core::CommandRecoveryState::Resolved,
                    "Recovered replacement by client_order_id",
                    replacement->id);
        }
        const auto target =
            find_order_by_id(command.target_order_id);
        if (!grace_elapsed) return std::nullopt;
        if (target != snapshot.orders.end() &&
            !TerminalOrderStatus(target->status) &&
            target->replaced_by.empty())
            return result(
                core::OrderCommandOutcome::RecoveryRejected,
                core::CommandRecoveryState::Rejected,
                "Authoritative target order was not replaced");
        return result(
            core::OrderCommandOutcome::Indeterminate,
            core::CommandRecoveryState::OperatorAttention,
            "Replacement could not be attributed from authoritative "
            "order state");
    }
    case core::OrderCommandKind::Cancel: {
        const auto target =
            find_order_by_id(command.target_order_id);
        if (target != snapshot.orders.end() &&
            CanceledOrderStatus(target->status))
            return result(
                core::OrderCommandOutcome::BrokerAccepted,
                core::CommandRecoveryState::Resolved,
                "Authoritative order state confirms cancellation",
                target->id);
        if (!grace_elapsed) return std::nullopt;
        if (target == snapshot.orders.end())
            return result(
                core::OrderCommandOutcome::Indeterminate,
                core::CommandRecoveryState::OperatorAttention,
                "Cancellation target is absent from authoritative "
                "order history");
        return result(
            core::OrderCommandOutcome::RecoveryRejected,
            core::CommandRecoveryState::Rejected,
            "Authoritative order state does not confirm cancellation",
            target->id);
    }
    case core::OrderCommandKind::ClosePosition: {
        const auto position = std::ranges::find_if(
            snapshot.positions,
            [&command](const core::PositionState& candidate) {
                return candidate.symbol ==
                           command.symbol_or_asset_id ||
                       candidate.asset_id ==
                           command.symbol_or_asset_id;
            });
        const bool full_close =
            !command.qty &&
            (!command.percentage ||
             *command.percentage == *core::Decimal::Parse("100"));
        if (full_close && position == snapshot.positions.end())
            return result(
                core::OrderCommandOutcome::BrokerAccepted,
                core::CommandRecoveryState::Resolved,
                "Authoritative position state confirms full close");
        if (!grace_elapsed) return std::nullopt;
        if (full_close)
            return result(
                core::OrderCommandOutcome::RecoveryRejected,
                core::CommandRecoveryState::Rejected,
                "Authoritative position remains open");
        return result(
            core::OrderCommandOutcome::Indeterminate,
            core::CommandRecoveryState::OperatorAttention,
            "Partial close requires operator review because the "
            "pre-dispatch quantity is unavailable");
    }
    case core::OrderCommandKind::CloseAllPositions:
        if (snapshot.positions.empty())
            return result(
                core::OrderCommandOutcome::BrokerAccepted,
                core::CommandRecoveryState::Resolved,
                "Authoritative state confirms all positions closed");
        if (!grace_elapsed) return std::nullopt;
        return result(
            core::OrderCommandOutcome::Indeterminate,
            core::CommandRecoveryState::OperatorAttention,
            "Some positions remain after close-all recovery");
    case core::OrderCommandKind::CancelAllOrders:
        if (std::ranges::none_of(
                snapshot.orders,
                [](const core::OrderState& order) {
                    return !TerminalOrderStatus(order.status);
                }))
            return result(
                core::OrderCommandOutcome::BrokerAccepted,
                core::CommandRecoveryState::Resolved,
                "Authoritative state confirms no open orders");
        if (!grace_elapsed) return std::nullopt;
        return result(
            core::OrderCommandOutcome::Indeterminate,
            core::CommandRecoveryState::OperatorAttention,
            "Open orders remain after cancel-all recovery");
    }
    return std::nullopt;
}

core::OrderCommandResult OrderExecutionService::Execute(
    core::NativeOrderCommand command) {
    const core::OrderCommandContext& context = Context(command);
    if (context.request_id.empty())
        return Rejected({}, core::OrderCommandOutcome::ValidationRejected,
                        "request_id is required");
    if (context.source.empty())
        return Rejected(context.request_id,
                        core::OrderCommandOutcome::ValidationRejected,
                        "command source is required");

    std::string client_order_id;
    if (auto* place = std::get_if<core::PlaceOrderCommand>(&command)) {
        if (place->order.client_order_id.empty())
            place->order.client_order_id =
                DerivedClientOrderId(context.request_id);
        client_order_id = place->order.client_order_id;
    } else if (auto* replace =
                   std::get_if<core::ReplaceOrderCommand>(&command)) {
        if (!replace->replacement.client_order_id)
            replace->replacement.client_order_id =
                DerivedClientOrderId(context.request_id);
        client_order_id =
            *replace->replacement.client_order_id;
    } else if (auto* bracket =
                   std::get_if<core::AmendBracketOrderCommand>(&command)) {
        for (std::size_t index = 0; index < bracket->amendments.size(); ++index)
            if (!bracket->amendments[index].replacement.client_order_id)
                bracket->amendments[index].replacement.client_order_id =
                    DerivedClientOrderId(context.request_id + "-" +
                                         std::to_string(index));
    }

    const core::OrderCommandRecord record{
        .request_id = context.request_id,
        .source = context.source,
        .kind = Kind(command),
        .account_id = context.account_id,
        .environment = context.environment,
        .generation = context.generation,
        .client_order_id = std::move(client_order_id),
        .created_at_ms = ToMilliseconds(clock_.Now()),
    };
    auto reservation = journal_.Reserve(record, command);
    if (!reservation)
        return Rejected(
            context.request_id, core::OrderCommandOutcome::Indeterminate,
            "Command journal reservation failed; broker was not called: " +
                reservation.error());
    if (reservation->reservation == core::CommandReservation::Duplicate) {
        if (reservation->existing_result)
            return *reservation->existing_result;
        return Rejected(
            context.request_id, core::OrderCommandOutcome::Indeterminate,
            "Duplicate command has no durable terminal result; reconcile by "
            "client_order_id before retrying");
    }

    auto complete = [this](core::OrderCommandResult result) {
        if (auto persisted = journal_.Complete(result); !persisted) {
            result.outcome = core::OrderCommandOutcome::Indeterminate;
            result.message =
                "Broker outcome could not be durably recorded: " +
                persisted.error();
            result.recovery_state =
                core::CommandRecoveryState::Pending;
            result.recovery_message =
                "Durable completion failed; authoritative recovery "
                "required";
        }
        return result;
    };
    auto mark_dispatch = [this, &context]()
        -> std::optional<core::OrderCommandResult> {
        if (auto marked =
                journal_.MarkDispatchStarted(context.request_id);
            !marked) {
            core::OrderCommandResult result = Rejected(
                context.request_id,
                core::OrderCommandOutcome::NotDispatched,
                "Broker was not called because the durable dispatch "
                "marker failed: " +
                    marked.error());
            result.recovery_state =
                core::CommandRecoveryState::Rejected;
            result.recovery_message = result.message;
            return result;
        }
        return std::nullopt;
    };

    const core::CoreSnapshot snapshot = core_.Snapshot();
    if (snapshot.safety_status ==
        core::SafetyStatus::Disconnected)
        return complete(Rejected(
            context.request_id,
            core::OrderCommandOutcome::SafetyRejected,
            "Order was not sent because the broker session is "
            "disconnected"));
    if (!snapshot.account || context.account_id.empty() ||
        snapshot.account->id != context.account_id)
        return complete(Rejected(
            context.request_id, core::OrderCommandOutcome::SafetyRejected,
            "Command account does not match the active broker account"));
    if (snapshot.environment != context.environment)
        return complete(Rejected(
            context.request_id, core::OrderCommandOutcome::SafetyRejected,
            "Command paper/live environment does not match active state"));
    if (snapshot.generation != context.generation)
        return complete(Rejected(
            context.request_id, core::OrderCommandOutcome::SafetyRejected,
            "Command connection generation is stale"));

    if (RequiresLiveSafety(command)) {
        if (snapshot.safety_status != core::SafetyStatus::Live ||
            !snapshot.reconciled || !snapshot.trading_permitted)
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::SafetyRejected,
                "This command requires LIVE, reconciled, trading-permitted "
                "state"));
        if (context.environment == core::AccountEnvironment::Live &&
            !context.live_trading_confirmed)
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::SafetyRejected,
                "Live trading requires explicit per-command confirmation"));
    } else if (snapshot.safety_status ==
                   core::SafetyStatus::Disconnected ||
               snapshot.safety_status == core::SafetyStatus::Connecting ||
               snapshot.safety_status == core::SafetyStatus::Error) {
        return complete(Rejected(
            context.request_id, core::OrderCommandOutcome::SafetyRejected,
            "Cancellation requires an identified broker session"));
    }

    broker::BrokerCommandResult broker_result;
    if (const auto* place =
            std::get_if<core::PlaceOrderCommand>(&command)) {
        if (market_data_) {
            const auto market =
                market_data_->Snapshot(place->order.symbol);
            if (market.trading_status &&
                market.trading_status->BlocksNewOrders())
                return complete(Rejected(
                    context.request_id,
                    core::OrderCommandOutcome::SafetyRejected,
                    "New order blocked by market trading status: " +
                        market.trading_status->status_message +
                        (market.trading_status
                                 ->reason_message.empty()
                             ? std::string{}
                             : " - " +
                                   market.trading_status
                                       ->reason_message)));
        }
        const auto errors = core::ValidateOrder(place->order);
        if (!errors.empty())
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                ValidationMessage(errors)));
        if (auto failed = mark_dispatch())
            return complete(std::move(*failed));
        broker_result = gateway_.PlaceOrder(place->order);
    } else if (const auto* cancel =
                   std::get_if<core::CancelOrderCommand>(&command)) {
        if (cancel->order_id.empty())
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "order_id is required"));
        if (auto failed = mark_dispatch())
            return complete(std::move(*failed));
        broker_result = gateway_.CancelOrder(cancel->order_id);
    } else if (const auto* replace =
                   std::get_if<core::ReplaceOrderCommand>(&command)) {
        const auto order = std::ranges::find_if(
            snapshot.orders, [replace](const core::OrderState& candidate) {
                return candidate.id == replace->order_id;
            });
        if (order == snapshot.orders.end())
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "Replacement target is absent from authoritative order state"));
        const auto errors =
            core::ValidateReplacement(*order, replace->replacement);
        if (!errors.empty())
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                ValidationMessage(errors)));
        if (auto failed = mark_dispatch())
            return complete(std::move(*failed));
        broker_result =
            gateway_.ReplaceOrder(replace->order_id, replace->replacement);
    } else if (const auto* bracket =
                   std::get_if<core::AmendBracketOrderCommand>(&command)) {
        const auto parent = std::ranges::find_if(
            snapshot.orders, [bracket](const core::OrderState& order) {
                return order.id == bracket->parent_order_id;
            });
        if (parent == snapshot.orders.end() || parent->order_class != "bracket" ||
            bracket->amendments.empty() || bracket->amendments.size() > 2U)
            return complete(Rejected(context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "Bracket amendment requires one or two legs of an authoritative bracket"));
        for (const auto& amendment : bracket->amendments) {
            const auto leg = std::ranges::find_if(snapshot.orders,
                [&amendment, bracket](const core::OrderState& order) {
                    return order.id == amendment.order_id &&
                           order.parent_order_id == bracket->parent_order_id;
                });
            if (leg == snapshot.orders.end()) return complete(Rejected(
                context.request_id, core::OrderCommandOutcome::ValidationRejected,
                "Bracket amendment leg is absent from authoritative group state"));
            const auto errors = core::ValidateReplacement(*leg, amendment.replacement);
            if (!errors.empty()) return complete(Rejected(context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                ValidationMessage(errors)));
        }
        if (auto failed = mark_dispatch()) return complete(std::move(*failed));
        std::vector<core::CommandItemResult> items;
        std::size_t accepted = 0;
        for (const auto& amendment : bracket->amendments) {
            const auto result = gateway_.ReplaceOrder(amendment.order_id,
                                                      amendment.replacement);
            accepted += result.disposition == broker::BrokerCommandDisposition::Accepted;
            items.push_back({.id = amendment.order_id, .http_status = result.http_status,
                .accepted = result.disposition == broker::BrokerCommandDisposition::Accepted,
                .message = result.message, .raw_response = result.raw_response});
        }
        return complete({.request_id = context.request_id,
            .outcome = accepted == bracket->amendments.size()
                ? core::OrderCommandOutcome::BrokerAccepted
                : accepted == 0 ? core::OrderCommandOutcome::BrokerRejected
                                : core::OrderCommandOutcome::PartiallyAccepted,
            .message = accepted == bracket->amendments.size()
                ? "Bracket legs accepted; reconciling order state"
                : "Bracket amendment only partially completed; operator review required",
            .items = std::move(items), .reconciliation_required = true,
            .recovery_state = accepted == bracket->amendments.size()
                ? core::CommandRecoveryState::NotRequired
                : core::CommandRecoveryState::OperatorAttention});
    } else if (const auto* close =
                   std::get_if<core::ClosePositionCommand>(&command)) {
        if (close->symbol_or_asset_id.empty())
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "symbol_or_asset_id is required"));
        if (close->qty && close->percentage)
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "qty and percentage are mutually exclusive"));
        const auto zero = core::Decimal::Zero();
        const auto hundred = *core::Decimal::Parse("100");
        if (close->qty && *close->qty <= zero)
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "qty must be greater than zero"));
        if (close->percentage &&
            (*close->percentage <= zero ||
             *close->percentage > hundred))
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "percentage must be greater than zero and at most 100"));
        const auto position = std::ranges::find_if(
            snapshot.positions,
            [close](const core::PositionState& candidate) {
                return candidate.symbol == close->symbol_or_asset_id ||
                       candidate.asset_id == close->symbol_or_asset_id;
            });
        if (position == snapshot.positions.end())
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "Close target is absent from authoritative position state"));
        if (position->asset_class != "us_equity")
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "V1.0 close position supports US equities only"));
        if (close->qty &&
            *close->qty > Absolute(position->qty_available))
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "qty exceeds authoritative available position quantity"));
        if (auto failed = mark_dispatch())
            return complete(std::move(*failed));
        std::vector<core::CommandItemResult> canceled_orders;
        if (close->cancel_open_orders) {
            for (const core::OrderState& order : snapshot.orders) {
                if (order.symbol != position->symbol ||
                    TerminalOrderStatus(order.status))
                    continue;
                const auto canceled = gateway_.CancelOrder(order.id);
                const bool accepted = canceled.disposition ==
                    broker::BrokerCommandDisposition::Accepted;
                canceled_orders.push_back({
                    .id = order.id,
                    .symbol = order.symbol,
                    .http_status = canceled.http_status,
                    .accepted = accepted,
                    .message = canceled.message,
                    .raw_response = canceled.raw_response,
                });
                if (!accepted) return complete({
                    .request_id = context.request_id,
                    .outcome = core::OrderCommandOutcome::PartiallyAccepted,
                    .message = "Position was not closed because one or more open orders could not be canceled",
                    .items = std::move(canceled_orders),
                    .reconciliation_required = true,
                    .recovery_state = core::CommandRecoveryState::OperatorAttention,
                    .recovery_message = "Reconcile open orders before retrying the position close",
                });
            }
            bool canceled_confirmed = false;
            for (int attempt = 0; attempt < 50 && !canceled_confirmed;
                 ++attempt) {
                canceled_confirmed = std::ranges::none_of(
                    core_.Snapshot().orders,
                    [position](const core::OrderState& order) {
                        return order.symbol == position->symbol &&
                               !TerminalOrderStatus(order.status);
                    });
                if (!canceled_confirmed)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!canceled_confirmed) return complete({
                .request_id = context.request_id,
                .outcome = core::OrderCommandOutcome::PartiallyAccepted,
                .message = "Close deferred: broker state has not confirmed cancellation of all open orders",
                .items = std::move(canceled_orders),
                .reconciliation_required = true,
                .recovery_state = core::CommandRecoveryState::OperatorAttention,
                .recovery_message = "Wait for reconciliation, then retry the close",
            });
        }
        broker_result = gateway_.ClosePosition(
            close->symbol_or_asset_id, close->qty, close->percentage);
        broker_result.items.insert(broker_result.items.begin(),
                                   canceled_orders.begin(), canceled_orders.end());
    } else if (const auto* close_all =
                   std::get_if<core::CloseAllPositionsCommand>(&command)) {
        if (std::ranges::any_of(
                snapshot.positions, [](const core::PositionState& position) {
                    return position.asset_class != "us_equity";
                }))
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "V1.0 close all is blocked while non-equity positions exist"));
        if (auto failed = mark_dispatch())
            return complete(std::move(*failed));
        broker_result =
            gateway_.CloseAllPositions(close_all->cancel_open_orders);
    } else {
        if (auto failed = mark_dispatch())
            return complete(std::move(*failed));
        broker_result = gateway_.CancelAllOrders();
    }

    core::OrderCommandOutcome outcome =
        core::OrderCommandOutcome::Indeterminate;
    if (broker_result.disposition ==
        broker::BrokerCommandDisposition::Accepted)
        outcome = core::OrderCommandOutcome::BrokerAccepted;
    else if (broker_result.disposition ==
             broker::BrokerCommandDisposition::Rejected)
        outcome = core::OrderCommandOutcome::BrokerRejected;
    else if (broker_result.disposition ==
             broker::BrokerCommandDisposition::PartiallyAccepted)
        outcome = core::OrderCommandOutcome::PartiallyAccepted;

    const bool recovery_required =
        outcome == core::OrderCommandOutcome::Indeterminate;
    return complete({
        .request_id = context.request_id,
        .outcome = outcome,
        .broker_order_id = std::move(broker_result.broker_order_id),
        .http_status = broker_result.http_status,
        .message = std::move(broker_result.message),
        .raw_response = std::move(broker_result.raw_response),
        .items = std::move(broker_result.items),
        .reconciliation_required =
            broker_result.reconciliation_required,
        .recovery_state =
            recovery_required
                ? core::CommandRecoveryState::Pending
                : core::CommandRecoveryState::NotRequired,
        .recovery_message =
            recovery_required
                ? "Awaiting authoritative broker-state recovery"
                : std::string{},
    });
}

}  // namespace tradebox::application
