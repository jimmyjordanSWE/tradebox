#include "tradebox/application/order_execution_service.h"
#include "tradebox/core/trading_core.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
        const core::NativeOrderCommand&) override {
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
        return core::ReservationResult{
            .reservation = core::CommandReservation::Reserved,
        };
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
    std::mutex mutex;
    std::unordered_map<std::string, core::OrderCommandRecord> records;
    std::unordered_map<std::string, core::OrderCommandResult> results;
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
              std::vector<core::OrderState> orders = {}) {
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
        .payload = core::PositionsSnapshotPayload{},
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

    EXPECT_EQ(result.outcome,
              core::OrderCommandOutcome::Indeterminate);
    EXPECT_TRUE(gateway.placed.empty());
}

}  // namespace
