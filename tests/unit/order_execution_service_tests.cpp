#include "tradebox/application/order_execution_service.h"
#include "tradebox/core/market_data_store.h"
#include "tradebox/core/trading_core.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace tradebox;

class FixedClock final : public core::IClock {
public:
    std::chrono::system_clock::time_point Now() const override {
        return std::chrono::system_clock::time_point{
            std::chrono::milliseconds{1'700'000'000'000}};
    }
};

class EventJournal final : public core::IEventJournal {
public:
    std::expected<void, core::CoreError> Append(
        const core::BrokerEvent&) override {
        return {};
    }
};

class CommandJournal final : public core::IOrderCommandJournal {
public:
    std::expected<core::ReservationResult, std::string> Reserve(
        const core::OrderCommandRecord& record,
        const core::NativeOrderCommand& command) override {
        if (fail_reserve)
            return std::unexpected("simulated reserve failure");
        std::scoped_lock lock(mutex);
        if (records.contains(record.request_id))
            return core::ReservationResult{
                .reservation = core::CommandReservation::Duplicate,
                .existing_result = results.contains(record.request_id)
                                       ? std::optional(results.at(
                                             record.request_id))
                                       : std::nullopt,
            };
        records.emplace(record.request_id, record);
        commands.emplace(record.request_id, command);
        return core::ReservationResult{
            .reservation = core::CommandReservation::Reserved,
        };
    }

    std::expected<void, std::string> MarkDispatchStarted(
        const std::string& request_id) override {
        if (fail_dispatch)
            return std::unexpected("simulated dispatch marker failure");
        std::scoped_lock lock(mutex);
        if (!records.contains(request_id))
            return std::unexpected("missing reservation");
        dispatched.insert(request_id);
        return {};
    }

    std::expected<std::vector<core::RecoverableOrderCommand>, std::string>
    Recoverable() override {
        std::scoped_lock lock(mutex);
        std::vector<core::RecoverableOrderCommand> pending;
        for (const auto& [request_id, record] : records) {
            const auto completed = results.find(request_id);
            if (completed != results.end()) {
                const bool pending_recovery =
                    completed->second.recovery_state ==
                        core::CommandRecoveryState::Pending ||
                    (completed->second.outcome ==
                         core::OrderCommandOutcome::Indeterminate &&
                     completed->second.recovery_state ==
                         core::CommandRecoveryState::NotRequired);
                if (!pending_recovery) continue;
            }
            core::RecoverableOrderCommand recovery{
                .record = record,
                .dispatch_started = dispatched.contains(request_id),
                .recovery_state =
                    core::CommandRecoveryState::Pending,
            };
            const auto command = commands.find(request_id);
            if (command != commands.end()) {
                std::visit(
                    [&recovery](const auto& typed) {
                        using T = std::decay_t<decltype(typed)>;
                        if constexpr (
                            std::is_same_v<T, core::CancelOrderCommand> ||
                            std::is_same_v<T, core::ReplaceOrderCommand>)
                            recovery.target_order_id = typed.order_id;
                        else if constexpr (std::is_same_v<
                                               T,
                                               core::ClosePositionCommand>) {
                            recovery.symbol_or_asset_id =
                                typed.symbol_or_asset_id;
                            recovery.qty = typed.qty;
                            recovery.percentage = typed.percentage;
                        } else if constexpr (std::is_same_v<
                                                  T,
                                                  core::CloseAllPositionsCommand>)
                            recovery.cancel_open_orders =
                                typed.cancel_open_orders;
                    },
                    command->second);
            }
            pending.push_back(std::move(recovery));
        }
        return pending;
    }

    std::expected<void, std::string> ResolveRecovery(
        const core::OrderCommandResult& result) override {
        if (fail_recovery)
            return std::unexpected("simulated recovery failure");
        std::scoped_lock lock(mutex);
        results.insert_or_assign(result.request_id, result);
        return {};
    }

    std::expected<void, std::string> Complete(
        const core::OrderCommandResult& result) override {
        if (fail_complete)
            return std::unexpected("simulated completion failure");
        std::scoped_lock lock(mutex);
        results.insert_or_assign(result.request_id, result);
        return {};
    }

    std::expected<core::OrderCommandLookup, std::string>
    Lookup(const std::string& request_id) override {
        std::scoped_lock lock(mutex);
        const auto found = results.find(request_id);
        if (found != results.end())
            return core::OrderCommandLookup{
                .exists = true,
                .terminal_result = found->second,
            };
        return core::OrderCommandLookup{
            .exists = records.contains(request_id),
        };
    }

    bool fail_reserve = false;
    bool fail_complete = false;
    bool fail_dispatch = false;
    bool fail_recovery = false;
    std::mutex mutex;
    std::unordered_map<std::string, core::OrderCommandRecord> records;
    std::unordered_map<std::string, core::NativeOrderCommand> commands;
    std::unordered_map<std::string, core::OrderCommandResult> results;
    std::unordered_set<std::string> dispatched;
};

class Gateway final : public broker::IOrderGateway {
public:
    broker::BrokerCommandResult PlaceOrder(
        const core::NativeOrderRequest& request) override {
        Enter();
        {
            std::scoped_lock lock(mutex);
            placed.push_back(request);
        }
        Exit();
        return next_result;
    }

    broker::BrokerCommandResult CancelOrder(
        const std::string& order_id) override {
        std::scoped_lock lock(mutex);
        canceled.push_back(order_id);
        return next_result;
    }

    broker::BrokerCommandResult ReplaceOrder(
        const std::string& order_id,
        const core::ReplaceOrderRequest& request) override {
        std::scoped_lock lock(mutex);
        replaced.push_back(order_id);
        replacement_requests.push_back(request);
        return next_result;
    }

    broker::BrokerCommandResult ClosePosition(
        const std::string& symbol_or_asset_id,
        const std::optional<core::Decimal>& qty,
        const std::optional<core::Decimal>& percentage) override {
        std::scoped_lock lock(mutex);
        closed_positions.push_back(symbol_or_asset_id);
        close_quantities.push_back(qty);
        close_percentages.push_back(percentage);
        return next_result;
    }

    broker::BrokerCommandResult CloseAllPositions(
        bool cancel_open_orders) override {
        std::scoped_lock lock(mutex);
        close_all_cancel_orders.push_back(cancel_open_orders);
        return next_result;
    }

    broker::BrokerCommandResult CancelAllOrders() override {
        std::scoped_lock lock(mutex);
        ++cancel_all_calls;
        return next_result;
    }

    void Enter() {
        const int now = ++in_flight;
        int observed = maximum_in_flight.load();
        while (now > observed &&
               !maximum_in_flight.compare_exchange_weak(observed, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    void Exit() { --in_flight; }

    broker::BrokerCommandResult next_result{
        .disposition = broker::BrokerCommandDisposition::Accepted,
        .http_status = 200,
        .broker_order_id = "broker-order-1",
        .message = "accepted",
        .raw_response = R"({"id":"broker-order-1"})",
    };
    std::mutex mutex;
    std::vector<core::NativeOrderRequest> placed;
    std::vector<std::string> canceled;
    std::vector<std::string> replaced;
    std::vector<core::ReplaceOrderRequest> replacement_requests;
    std::vector<std::string> closed_positions;
    std::vector<std::optional<core::Decimal>> close_quantities;
    std::vector<std::optional<core::Decimal>> close_percentages;
    std::vector<bool> close_all_cancel_orders;
    int cancel_all_calls = 0;
    std::atomic<int> in_flight = 0;
    std::atomic<int> maximum_in_flight = 0;
};

core::BrokerEvent Event(core::BrokerEventKind kind) {
    return {
        .kind = kind,
        .generation = core::ConnectionGeneration{1},
    };
}

void MakeLive(core::TradingCore& core,
              std::vector<core::OrderState> orders = {},
              std::vector<core::PositionState> positions = {}) {
    ASSERT_TRUE(
        core.Ingest(Event(core::BrokerEventKind::ConnectionAttemptStarted)));
    ASSERT_TRUE(core.Ingest(Event(core::BrokerEventKind::Authorized)));
    ASSERT_TRUE(
        core.Ingest(Event(core::BrokerEventKind::TradeUpdatesAcknowledged)));
    core::AccountState account;
    account.id = "account-1";
    account.status = "ACTIVE";
    ASSERT_TRUE(core.Ingest({
        .kind = core::BrokerEventKind::AccountSnapshot,
        .generation = core::ConnectionGeneration{1},
        .payload = core::AccountSnapshotPayload{.account = account},
    }));
    ASSERT_TRUE(core.Ingest({
        .kind = core::BrokerEventKind::PositionsSnapshot,
        .generation = core::ConnectionGeneration{1},
        .payload =
            core::PositionsSnapshotPayload{
                .positions = std::move(positions)},
    }));
    ASSERT_TRUE(core.Ingest({
        .kind = core::BrokerEventKind::OrdersSnapshot,
        .generation = core::ConnectionGeneration{1},
        .payload =
            core::OrdersSnapshotPayload{.orders = std::move(orders)},
    }));
    ASSERT_EQ(core.Snapshot().safety_status, core::SafetyStatus::Live);
}

core::PlaceOrderCommand Place(std::string request_id) {
    return {
        .context =
            {
                .request_id = std::move(request_id),
                .source = "unit-test",
                .account_id = "account-1",
                .environment = core::AccountEnvironment::Paper,
                .generation = core::ConnectionGeneration{1},
            },
        .order =
            {
                .asset_class = core::AssetClass::Equity,
                .symbol = "AAPL",
                .qty = *core::Decimal::Parse("0.123456789"),
                .side = core::OrderSide::Buy,
                .type = core::OrderType::Market,
                .time_in_force = core::TimeInForce::Day,
            },
    };
}

core::OrderCommandContext Context(std::string request_id) {
    return {
        .request_id = std::move(request_id),
        .source = "unit-test",
        .account_id = "account-1",
        .environment = core::AccountEnvironment::Paper,
        .generation = core::ConnectionGeneration{1},
    };
}

core::PositionState EquityPosition() {
    return {
        .asset_id = "asset-aapl",
        .symbol = "AAPL",
        .asset_class = "us_equity",
        .qty = *core::Decimal::Parse("10"),
        .qty_available = *core::Decimal::Parse("8"),
    };
}

core::OrderCommandRecord RecoveryRecord(
    std::string request_id,
    core::OrderCommandKind kind,
    std::string client_order_id = {}) {
    return {
        .request_id = std::move(request_id),
        .source = "recovery-test",
        .kind = kind,
        .account_id = "account-1",
        .environment = core::AccountEnvironment::Paper,
        .generation = core::ConnectionGeneration{1},
        .client_order_id = std::move(client_order_id),
        .created_at_ms = 1'700'000'000'000,
    };
}

std::optional<core::OrderCommandResult> WaitForTerminal(
    application::OrderExecutionService& service,
    const std::string& request_id) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto lookup = service.Lookup(request_id);
        if (lookup && lookup->terminal_result &&
            lookup->terminal_result->recovery_state !=
                core::CommandRecoveryState::Pending)
            return lookup->terminal_result;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::nullopt;
}

TEST(OrderExecutionService, DurablyReservesBeforeCallingGateway) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core);
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    const core::OrderCommandResult result =
        service.Submit(Place("request-1")).get();

    EXPECT_EQ(result.outcome, core::OrderCommandOutcome::BrokerAccepted);
    ASSERT_EQ(gateway.placed.size(), 1U);
    EXPECT_EQ(gateway.placed.front().client_order_id, "tb-request-1");
    EXPECT_TRUE(command_journal.records.contains("request-1"));
    EXPECT_TRUE(command_journal.results.contains("request-1"));
    const auto lookup = service.Lookup("request-1");
    ASSERT_TRUE(lookup);
    EXPECT_TRUE(lookup->exists);
    ASSERT_TRUE(lookup->terminal_result);
    EXPECT_EQ(lookup->terminal_result->broker_order_id,
              "broker-order-1");
}

TEST(OrderExecutionService,
     DisconnectedSubmissionReturnsAnImmediateTypedResponse) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    auto future = service.Submit(Place("while-disconnected"));

    EXPECT_EQ(
        future.wait_for(std::chrono::milliseconds(0)),
        std::future_status::ready);
    const auto result = future.get();
    EXPECT_EQ(result.outcome,
              core::OrderCommandOutcome::SafetyRejected);
    EXPECT_NE(result.message.find("disconnected"),
              std::string::npos);
    EXPECT_TRUE(gateway.placed.empty());
    EXPECT_TRUE(command_journal.records.empty());
}

TEST(OrderExecutionService, DuplicateRequestReturnsDurableResultOnce) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core);
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    const auto first = service.Submit(Place("same-request")).get();
    const auto duplicate = service.Submit(Place("same-request")).get();

    EXPECT_EQ(first.outcome, core::OrderCommandOutcome::BrokerAccepted);
    EXPECT_EQ(duplicate.broker_order_id, first.broker_order_id);
    EXPECT_EQ(gateway.placed.size(), 1U);
}

TEST(OrderExecutionService, JournalFailurePreventsBrokerCall) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core);
    Gateway gateway;
    CommandJournal command_journal;
    command_journal.fail_reserve = true;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    const auto result = service.Submit(Place("journal-failure")).get();

    EXPECT_EQ(result.outcome, core::OrderCommandOutcome::Indeterminate);
    EXPECT_TRUE(gateway.placed.empty());
}

TEST(OrderExecutionService, DispatchMarkerFailurePreventsBrokerCall) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core);
    Gateway gateway;
    CommandJournal command_journal;
    command_journal.fail_dispatch = true;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    const auto result =
        service.Submit(Place("dispatch-marker-failure")).get();

    EXPECT_EQ(result.outcome,
              core::OrderCommandOutcome::NotDispatched);
    EXPECT_EQ(result.recovery_state,
              core::CommandRecoveryState::Rejected);
    EXPECT_TRUE(gateway.placed.empty());
}

TEST(OrderExecutionService, StaleGenerationCannotReachBroker) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core);
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);
    auto command = Place("stale");
    command.context.generation = core::ConnectionGeneration{0};

    const auto result = service.Submit(std::move(command)).get();

    EXPECT_EQ(result.outcome, core::OrderCommandOutcome::SafetyRejected);
    EXPECT_TRUE(gateway.placed.empty());
}

TEST(OrderExecutionService, LiveAccountRequiresExplicitConfirmation) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    ASSERT_TRUE(core.Submit(core::ConnectAccount{
        core::AccountEnvironment::Live}));
    MakeLive(core);
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);
    auto command = Place("live-unconfirmed");
    command.context.environment = core::AccountEnvironment::Live;

    const auto result = service.Submit(std::move(command)).get();

    EXPECT_EQ(result.outcome, core::OrderCommandOutcome::SafetyRejected);
    EXPECT_TRUE(gateway.placed.empty());
}

TEST(OrderExecutionService, TradingHaltBlocksNewOrderBeforeBrokerCall) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core);
    core::MarketDataStore market_data;
    market_data.Ingest(core::TradingStatusReceived{
        .status = {
            .instrument_id = "asset-aapl",
            .symbol = "AAPL",
            .state =
                core::SecurityTradingState::Halted,
            .status_code = "H",
            .status_message = "Trading Halt",
            .reason_code = "T12",
            .reason_message =
                "Information requested by NASDAQ",
            .event_time_ns = 100,
            .received_at_ms = 10,
        },
    });
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock,
        &market_data);

    const auto result =
        service.Submit(Place("halted-order")).get();

    EXPECT_EQ(result.outcome,
              core::OrderCommandOutcome::SafetyRejected);
    EXPECT_NE(result.message.find("Trading Halt"),
              std::string::npos);
    EXPECT_TRUE(gateway.placed.empty());
}

TEST(OrderExecutionService, SerializesCommandsFromConcurrentCallers) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core);
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);
    std::vector<std::future<core::OrderCommandResult>> results;
    for (int index = 0; index < 8; ++index)
        results.push_back(service.Submit(
            Place("parallel-" + std::to_string(index))));
    for (auto& result : results)
        EXPECT_EQ(result.get().outcome,
                  core::OrderCommandOutcome::BrokerAccepted);

    EXPECT_EQ(gateway.maximum_in_flight.load(), 1);
    EXPECT_EQ(gateway.placed.size(), 8U);
}

TEST(OrderExecutionService, CancelRemainsAvailableWhenStreamIsStale) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core);
    ASSERT_TRUE(
        core.Ingest(Event(core::BrokerEventKind::Disconnected)));
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    const auto result =
        service
            .Submit(core::CancelOrderCommand{
                .context =
                    {
                        .request_id = "emergency-cancel",
                        .source = "unit-test",
                        .account_id = "account-1",
                        .environment =
                            core::AccountEnvironment::Paper,
                        .generation =
                            core::ConnectionGeneration{1},
                    },
                .order_id = "order-1",
            })
            .get();

    EXPECT_EQ(result.outcome,
              core::OrderCommandOutcome::BrokerAccepted);
    ASSERT_EQ(gateway.canceled.size(), 1U);
    EXPECT_EQ(gateway.canceled.front(), "order-1");
}

TEST(OrderExecutionService, ReplacementGetsIdempotentClientOrderId) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    core::OrderState order;
    order.id = "order-1";
    order.asset_class = "us_equity";
    order.status = "new";
    order.type = "limit";
    order.order_class = "simple";
    order.qty = *core::Decimal::Parse("10");
    MakeLive(core, {order});
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    const auto result =
        service
            .Submit(core::ReplaceOrderCommand{
                .context =
                    {
                        .request_id = "replace-1",
                        .source = "unit-test",
                        .account_id = "account-1",
                        .environment =
                            core::AccountEnvironment::Paper,
                        .generation =
                            core::ConnectionGeneration{1},
                    },
                .order_id = "order-1",
                .replacement =
                    {.limit_price =
                         *core::Decimal::Parse("200")},
            })
            .get();

    EXPECT_EQ(result.outcome,
              core::OrderCommandOutcome::BrokerAccepted);
    ASSERT_EQ(gateway.replacement_requests.size(), 1U);
    ASSERT_TRUE(
        gateway.replacement_requests.front().client_order_id);
    EXPECT_EQ(*gateway.replacement_requests.front().client_order_id,
              "tb-replace-1");
}

TEST(OrderExecutionService,
     CompletionFailureMakesKnownBrokerResponseIndeterminate) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core);
    Gateway gateway;
    CommandJournal command_journal;
    command_journal.fail_complete = true;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    const auto result =
        service.Submit(Place("completion-failure")).get();

    EXPECT_EQ(result.outcome,
              core::OrderCommandOutcome::Indeterminate);
    EXPECT_EQ(gateway.placed.size(), 1U);
}

TEST(OrderExecutionService,
     IncompleteDuplicateIsNotAutomaticallyResubmitted) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core);
    Gateway gateway;
    CommandJournal command_journal;
    command_journal.records.emplace(
        "pending-command",
        core::OrderCommandRecord{
            .request_id = "pending-command",
        });
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    const auto result =
        service.Submit(Place("pending-command")).get();

    EXPECT_TRUE(
        result.outcome == core::OrderCommandOutcome::Indeterminate ||
        result.outcome == core::OrderCommandOutcome::NotDispatched);
    EXPECT_TRUE(gateway.placed.empty());
    const auto recovered =
        WaitForTerminal(service, "pending-command");
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->outcome,
              core::OrderCommandOutcome::NotDispatched);
    EXPECT_EQ(recovered->recovery_state,
              core::CommandRecoveryState::Rejected);
}

TEST(OrderExecutionService,
     StartupRecoveryFindsPlacedOrderByClientOrderIdWithoutResubmitting) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    core::OrderState broker_order;
    broker_order.id = "broker-recovered";
    broker_order.client_order_id = "tb-recovered-place";
    broker_order.status = "new";
    MakeLive(core, {broker_order});
    Gateway gateway;
    CommandJournal command_journal;
    auto command = Place("recovered-place");
    command.order.client_order_id = "tb-recovered-place";
    command_journal.records.emplace(
        "recovered-place",
        RecoveryRecord(
            "recovered-place", core::OrderCommandKind::Place,
            "tb-recovered-place"));
    command_journal.commands.emplace(
        "recovered-place", command);
    command_journal.dispatched.insert("recovered-place");

    application::OrderExecutionService service(
        core, gateway, command_journal, clock);
    const auto recovered =
        WaitForTerminal(service, "recovered-place");

    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->outcome,
              core::OrderCommandOutcome::BrokerAccepted);
    EXPECT_EQ(recovered->recovery_state,
              core::CommandRecoveryState::Resolved);
    EXPECT_EQ(recovered->broker_order_id, "broker-recovered");
    EXPECT_TRUE(gateway.placed.empty());
}

TEST(OrderExecutionService,
     StartupRecoveryResolvesCancelAndReplaceFromAuthoritativeOrders) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    core::OrderState canceled;
    canceled.id = "cancel-target";
    canceled.status = "canceled";
    core::OrderState replacement;
    replacement.id = "replacement-order";
    replacement.client_order_id = "tb-replace-recovery";
    replacement.status = "new";
    MakeLive(core, {canceled, replacement});
    Gateway gateway;
    CommandJournal command_journal;

    const core::CancelOrderCommand cancel{
        .context = Context("cancel-recovery"),
        .order_id = "cancel-target",
    };
    command_journal.records.emplace(
        "cancel-recovery",
        RecoveryRecord(
            "cancel-recovery", core::OrderCommandKind::Cancel));
    command_journal.commands.emplace("cancel-recovery", cancel);
    command_journal.dispatched.insert("cancel-recovery");

    const core::ReplaceOrderCommand replace{
        .context = Context("replace-recovery"),
        .order_id = "replace-target",
        .replacement = {
            .client_order_id = "tb-replace-recovery",
        },
    };
    command_journal.records.emplace(
        "replace-recovery",
        RecoveryRecord(
            "replace-recovery", core::OrderCommandKind::Replace,
            "tb-replace-recovery"));
    command_journal.commands.emplace("replace-recovery", replace);
    command_journal.dispatched.insert("replace-recovery");

    application::OrderExecutionService service(
        core, gateway, command_journal, clock);
    const auto cancel_result =
        WaitForTerminal(service, "cancel-recovery");
    const auto replace_result =
        WaitForTerminal(service, "replace-recovery");

    ASSERT_TRUE(cancel_result);
    ASSERT_TRUE(replace_result);
    EXPECT_EQ(cancel_result->recovery_state,
              core::CommandRecoveryState::Resolved);
    EXPECT_EQ(replace_result->recovery_state,
              core::CommandRecoveryState::Resolved);
    EXPECT_TRUE(gateway.canceled.empty());
    EXPECT_TRUE(gateway.replaced.empty());
}

TEST(OrderExecutionService,
     RecoveryRejectsDispatchedPlaceOnlyAfterAuthoritativeAbsence) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core);
    Gateway gateway;
    CommandJournal command_journal;
    auto command = Place("absent-place");
    command.order.client_order_id = "tb-absent-place";
    auto record = RecoveryRecord(
        "absent-place", core::OrderCommandKind::Place,
        "tb-absent-place");
    record.created_at_ms -= 60'000;
    command_journal.records.emplace("absent-place", record);
    command_journal.commands.emplace("absent-place", command);
    command_journal.dispatched.insert("absent-place");

    application::OrderExecutionService service(
        core, gateway, command_journal, clock);
    const auto recovered =
        WaitForTerminal(service, "absent-place");

    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->outcome,
              core::OrderCommandOutcome::RecoveryRejected);
    EXPECT_EQ(recovered->recovery_state,
              core::CommandRecoveryState::Rejected);
    EXPECT_TRUE(gateway.placed.empty());
}

TEST(OrderExecutionService,
     AmbiguousPartialCloseRequiresExplicitOperatorAttention) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core, {}, {EquityPosition()});
    Gateway gateway;
    CommandJournal command_journal;
    const core::ClosePositionCommand command{
        .context = Context("partial-close-recovery"),
        .symbol_or_asset_id = "AAPL",
        .qty = *core::Decimal::Parse("2"),
    };
    auto record = RecoveryRecord(
        "partial-close-recovery",
        core::OrderCommandKind::ClosePosition);
    record.created_at_ms -= 60'000;
    command_journal.records.emplace(
        "partial-close-recovery", record);
    command_journal.commands.emplace(
        "partial-close-recovery", command);
    command_journal.dispatched.insert(
        "partial-close-recovery");

    application::OrderExecutionService service(
        core, gateway, command_journal, clock);
    const auto recovered =
        WaitForTerminal(service, "partial-close-recovery");

    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->outcome,
              core::OrderCommandOutcome::Indeterminate);
    EXPECT_EQ(recovered->recovery_state,
              core::CommandRecoveryState::OperatorAttention);
    EXPECT_TRUE(gateway.closed_positions.empty());
}

TEST(OrderExecutionService,
     ClosesAuthoritativeEquityByFullQuantityOrPercentage) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core, {}, {EquityPosition()});
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    EXPECT_EQ(
        service
            .Submit(core::ClosePositionCommand{
                .context = Context("close-full"),
                .symbol_or_asset_id = "AAPL",
            })
            .get()
            .outcome,
        core::OrderCommandOutcome::BrokerAccepted);
    EXPECT_EQ(
        service
            .Submit(core::ClosePositionCommand{
                .context = Context("close-qty"),
                .symbol_or_asset_id = "asset-aapl",
                .qty = *core::Decimal::Parse("3.25"),
            })
            .get()
            .outcome,
        core::OrderCommandOutcome::BrokerAccepted);
    EXPECT_EQ(
        service
            .Submit(core::ClosePositionCommand{
                .context = Context("close-percent"),
                .symbol_or_asset_id = "AAPL",
                .percentage = *core::Decimal::Parse("40"),
            })
            .get()
            .outcome,
        core::OrderCommandOutcome::BrokerAccepted);

    ASSERT_EQ(gateway.closed_positions.size(), 3U);
    ASSERT_TRUE(gateway.close_quantities[1]);
    EXPECT_EQ(gateway.close_quantities[1]->ToString(), "3.25");
    ASSERT_TRUE(gateway.close_percentages[2]);
    EXPECT_EQ(gateway.close_percentages[2]->ToString(), "40");
}

TEST(OrderExecutionService, RejectsInvalidOrUnavailableCloseAmounts) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core, {}, {EquityPosition()});
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    const auto both = service.Submit(core::ClosePositionCommand{
        .context = Context("close-both"),
        .symbol_or_asset_id = "AAPL",
        .qty = *core::Decimal::Parse("1"),
        .percentage = *core::Decimal::Parse("10"),
    }).get();
    const auto too_many = service.Submit(core::ClosePositionCommand{
        .context = Context("close-too-many"),
        .symbol_or_asset_id = "AAPL",
        .qty = *core::Decimal::Parse("9"),
    }).get();
    const auto too_large = service.Submit(core::ClosePositionCommand{
        .context = Context("close-too-large"),
        .symbol_or_asset_id = "AAPL",
        .percentage = *core::Decimal::Parse("101"),
    }).get();

    EXPECT_EQ(both.outcome,
              core::OrderCommandOutcome::ValidationRejected);
    EXPECT_EQ(too_many.outcome,
              core::OrderCommandOutcome::ValidationRejected);
    EXPECT_EQ(too_large.outcome,
              core::OrderCommandOutcome::ValidationRejected);
    EXPECT_TRUE(gateway.closed_positions.empty());
}

TEST(OrderExecutionService,
     BulkPartialSuccessIsExplicitDurableAndNotResubmitted) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core, {}, {EquityPosition()});
    Gateway gateway;
    gateway.next_result = {
        .disposition =
            broker::BrokerCommandDisposition::PartiallyAccepted,
        .http_status = 207,
        .message = "partial",
        .raw_response = R"([{"id":"one"},{"id":"two"}])",
        .items = {
            {.id = "one", .http_status = 200, .accepted = true},
            {.id = "two", .http_status = 500, .accepted = false},
        },
        .reconciliation_required = true,
    };
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);
    const core::CloseAllPositionsCommand command{
        .context = Context("close-all-partial"),
        .cancel_open_orders = true,
    };

    const auto first = service.Submit(command).get();
    const auto duplicate = service.Submit(command).get();

    EXPECT_EQ(first.outcome,
              core::OrderCommandOutcome::PartiallyAccepted);
    EXPECT_TRUE(first.reconciliation_required);
    ASSERT_EQ(first.items.size(), 2U);
    EXPECT_FALSE(first.items[1].accepted);
    EXPECT_EQ(duplicate.items.size(), 2U);
    EXPECT_EQ(gateway.close_all_cancel_orders.size(), 1U);
}

TEST(OrderExecutionService,
     EmergencyCancelAllWorksWhenStreamIsStaleButRejectsStaleGeneration) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core);
    ASSERT_TRUE(core.Ingest(Event(core::BrokerEventKind::Disconnected)));
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    const auto accepted = service.Submit(core::CancelAllOrdersCommand{
        .context = Context("cancel-all"),
    }).get();
    auto stale = Context("cancel-all-stale-generation");
    stale.generation = core::ConnectionGeneration{0};
    const auto rejected = service.Submit(core::CancelAllOrdersCommand{
        .context = std::move(stale),
    }).get();

    EXPECT_EQ(accepted.outcome,
              core::OrderCommandOutcome::BrokerAccepted);
    EXPECT_EQ(rejected.outcome,
              core::OrderCommandOutcome::SafetyRejected);
    EXPECT_EQ(gateway.cancel_all_calls, 1);
}

TEST(OrderExecutionService,
     AmbiguousCloseRequiresReconciliationAndRetainsOutcome) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    MakeLive(core, {}, {EquityPosition()});
    Gateway gateway;
    gateway.next_result = {
        .disposition =
            broker::BrokerCommandDisposition::Indeterminate,
        .message = "timeout",
        .reconciliation_required = true,
    };
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    const auto result = service.Submit(core::ClosePositionCommand{
        .context = Context("close-timeout"),
        .symbol_or_asset_id = "AAPL",
    }).get();

    EXPECT_EQ(result.outcome,
              core::OrderCommandOutcome::Indeterminate);
    EXPECT_TRUE(result.reconciliation_required);
    ASSERT_TRUE(command_journal.results.contains("close-timeout"));
    EXPECT_TRUE(command_journal.results.at("close-timeout")
                    .reconciliation_required);
}

TEST(OrderExecutionService, CloseAllDoesNotLiquidateNonEquityAssets) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    auto crypto = EquityPosition();
    crypto.asset_class = "crypto";
    MakeLive(core, {}, {crypto});
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);

    const auto result = service.Submit(core::CloseAllPositionsCommand{
        .context = Context("close-all-mixed-assets"),
    }).get();

    EXPECT_EQ(result.outcome,
              core::OrderCommandOutcome::ValidationRejected);
    EXPECT_TRUE(gateway.close_all_cancel_orders.empty());
}

TEST(OrderExecutionService, LiveCloseAllRequiresExplicitConfirmation) {
    EventJournal event_journal;
    FixedClock clock;
    core::TradingCore core(event_journal, clock);
    ASSERT_TRUE(core.Submit(core::ConnectAccount{
        core::AccountEnvironment::Live}));
    MakeLive(core, {}, {EquityPosition()});
    Gateway gateway;
    CommandJournal command_journal;
    application::OrderExecutionService service(
        core, gateway, command_journal, clock);
    auto context = Context("live-close-all");
    context.environment = core::AccountEnvironment::Live;

    const auto result = service.Submit(core::CloseAllPositionsCommand{
        .context = std::move(context),
        .cancel_open_orders = true,
    }).get();

    EXPECT_EQ(result.outcome,
              core::OrderCommandOutcome::SafetyRejected);
    EXPECT_TRUE(gateway.close_all_cancel_orders.empty());
}

}  // namespace
