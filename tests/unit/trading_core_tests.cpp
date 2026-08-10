#include "tradebox/core/trading_core.h"

#include <gtest/gtest.h>

#include <chrono>
#include <barrier>
#include <expected>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace tradebox::core;

Decimal D(std::string_view text) {
    return *Decimal::Parse(text);
}

class FixedClock final : public IClock {
public:
    std::chrono::system_clock::time_point Now() const override {
        return now;
    }

    std::chrono::system_clock::time_point now{
        std::chrono::seconds{1'700'000'000}};
};

class MemoryJournal final : public IEventJournal {
public:
    std::expected<void, CoreError> Append(
        const BrokerEvent& event) override {
        if (fail_writes) {
            return std::unexpected(CoreError{
                .code = CoreErrorCode::JournalFailure,
                .message = "simulated journal failure",
            });
        }
        events.push_back(event);
        return {};
    }

    bool fail_writes = false;
    std::vector<BrokerEvent> events;
};

BrokerEvent Event(BrokerEventKind kind, std::uint64_t generation) {
    return BrokerEvent{
        .kind = kind,
        .generation = ConnectionGeneration{generation},
    };
}

BrokerEvent AccountEvent(std::uint64_t generation) {
    AccountState account;
    account.id = "account-id";
    account.account_number = "PA123";
    account.status = "ACTIVE";
    return BrokerEvent{
        .kind = BrokerEventKind::AccountSnapshot,
        .generation = ConnectionGeneration{generation},
        .payload = AccountSnapshotPayload{.account = std::move(account)},
    };
}

BrokerEvent OrdersEvent(std::uint64_t generation,
                        std::vector<OrderState> orders = {}) {
    return BrokerEvent{
        .kind = BrokerEventKind::OrdersSnapshot,
        .generation = ConnectionGeneration{generation},
        .payload = OrdersSnapshotPayload{
            .orders = std::move(orders),
            .received_at_ms = 1234,
        },
    };
}

BrokerEvent PositionsEvent(
    std::uint64_t generation,
    std::vector<PositionState> positions = {}) {
    return BrokerEvent{
        .kind = BrokerEventKind::PositionsSnapshot,
        .generation = ConnectionGeneration{generation},
        .payload = PositionsSnapshotPayload{
            .positions = std::move(positions),
            .received_at_ms = 1234,
        },
    };
}

TEST(TradingCore, StartsDisconnected) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);

    const CoreSnapshot snapshot = core.Snapshot();
    EXPECT_EQ(snapshot.safety_status, SafetyStatus::Disconnected);
    EXPECT_EQ(snapshot.generation.value, 0U);
    EXPECT_FALSE(snapshot.authenticated);
    EXPECT_FALSE(snapshot.trade_updates_acknowledged);
    EXPECT_FALSE(snapshot.initial_snapshot_loaded);
    EXPECT_FALSE(snapshot.reconciled);
}

TEST(TradingCore, RetainsAdvancedOrderLegsFromAuthoritativeSnapshot) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);

    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));
    OrderState parent;
    parent.id = "entry";
    parent.status = "filled";
    parent.order_class = "bracket";
    OrderState target;
    target.id = "target";
    target.parent_order_id = "entry";
    target.status = "new";
    OrderState stop;
    stop.id = "stop";
    stop.parent_order_id = "entry";
    stop.status = "held";

    ASSERT_TRUE(core.Ingest(OrdersEvent(1, {parent, target, stop})));
    const CoreSnapshot snapshot = core.Snapshot();
    ASSERT_EQ(snapshot.orders.size(), 3U);
    const auto target_found = std::ranges::find_if(
        snapshot.orders, [](const OrderState& order) {
            return order.id == "target";
        });
    ASSERT_NE(target_found, snapshot.orders.end());
    EXPECT_EQ(target_found->parent_order_id, "entry");
}

TEST(TradingCore, AvoidsCopyingUnchangedSnapshots) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);

    EXPECT_FALSE(core.SnapshotAfter(core.Snapshot().revision));
    ASSERT_TRUE(core.Submit(
        ConnectAccount{AccountEnvironment::Paper}));
    const auto changed = core.SnapshotAfter(0);
    ASSERT_TRUE(changed);
    EXPECT_GT(changed->revision, 0U);
    EXPECT_FALSE(core.SnapshotAfter(changed->revision));
}

TEST(TradingCore, RequiresEverySafetyBarrierBeforeLive) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);

    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));
    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Authorized, 1)));
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::TradeUpdatesAcknowledged, 1)));
    ASSERT_TRUE(core.Ingest(AccountEvent(1)));
    ASSERT_TRUE(core.Ingest(PositionsEvent(1)));
    ASSERT_TRUE(core.Ingest(OrdersEvent(1)));

    const CoreSnapshot live = core.Snapshot();
    EXPECT_EQ(live.safety_status, SafetyStatus::Live);
    EXPECT_TRUE(live.authenticated);
    EXPECT_TRUE(live.trade_updates_acknowledged);
    EXPECT_TRUE(live.initial_snapshot_loaded);
    EXPECT_TRUE(live.reconciled);
}

TEST(TradingCore, RejectsEventsFromOlderConnectionGenerations) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);

    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 2)));
    const CoreSnapshot before = core.Snapshot();

    const auto result =
        core.Ingest(Event(BrokerEventKind::Authorized, 1));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, CoreErrorCode::StaleGeneration);
    const CoreSnapshot after = core.Snapshot();
    EXPECT_EQ(after.revision, before.revision);
    EXPECT_FALSE(after.authenticated);
}

TEST(TradingCore, JournalFailurePreventsStateMutation) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    journal.fail_writes = true;

    const auto result = core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, CoreErrorCode::JournalFailure);
    EXPECT_EQ(core.Snapshot().generation.value, 0U);
    EXPECT_EQ(core.Snapshot().revision, 0U);
}

TEST(TradingCore, DisconnectClearsLiveBarriersAndMarksStateStale) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);

    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));
    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Authorized, 1)));
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::TradeUpdatesAcknowledged, 1)));
    ASSERT_TRUE(core.Ingest(AccountEvent(1)));
    ASSERT_TRUE(core.Ingest(PositionsEvent(1)));
    ASSERT_TRUE(core.Ingest(OrdersEvent(1)));
    ASSERT_EQ(core.Snapshot().safety_status, SafetyStatus::Live);

    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Disconnected, 1)));
    const CoreSnapshot snapshot = core.Snapshot();
    EXPECT_EQ(snapshot.safety_status, SafetyStatus::Stale);
    EXPECT_FALSE(snapshot.authenticated);
    EXPECT_FALSE(snapshot.trade_updates_acknowledged);
    EXPECT_FALSE(snapshot.initial_snapshot_loaded);
    EXPECT_FALSE(snapshot.reconciled);
}

// A routine account-stream disconnect is expected during normal operation
// (idle timeout, transient network blip). The REST /v2/account snapshot is
// not invalidated by a stream flap, so it remains visible while the stream
// reconnects rather than clearing the account data from the UI.
TEST(TradingCore, StreamFlapClearsAuthButKeepsAccountData) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);

    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));
    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Authorized, 1)));
    ASSERT_TRUE(core.Ingest(AccountEvent(1)));
    ASSERT_TRUE(core.Snapshot().account.has_value());
    ASSERT_TRUE(core.Snapshot().authenticated);

    // The account stream drops (routine flap) and publishes Disconnected.
    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Disconnected, 1)));
    const CoreSnapshot flap = core.Snapshot();
    EXPECT_EQ(flap.safety_status, SafetyStatus::Stale);
    EXPECT_FALSE(flap.authenticated);
    // REST-fetched account data must survive a stream flap so the account
    // menu can still show buying power etc. while reconnecting.
    EXPECT_TRUE(flap.account.has_value());
    EXPECT_TRUE(flap.account_snapshot_loaded);
}

// An explicit disconnect (user action) ends the session; the account snapshot
// is connection-scoped truth and must not linger after the session is gone.
TEST(TradingCore, ExplicitDisconnectClearsAccountSnapshot) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);

    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));
    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Authorized, 1)));
    ASSERT_TRUE(core.Ingest(AccountEvent(1)));
    ASSERT_TRUE(core.Snapshot().account.has_value());

    ASSERT_TRUE(core.Submit(DisconnectAccount{}));
    const CoreSnapshot snapshot = core.Snapshot();
    EXPECT_EQ(snapshot.safety_status, SafetyStatus::Disconnected);
    EXPECT_FALSE(snapshot.authenticated);
    // Stale account data must not present as an active connection.
    EXPECT_FALSE(snapshot.account.has_value());
}

TEST(TradingCore, FailureResetsConnectionAndClearsAccount) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);

    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));
    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Authorized, 1)));
    ASSERT_TRUE(core.Ingest(AccountEvent(1)));
    ASSERT_TRUE(core.Snapshot().account.has_value());

    ASSERT_TRUE(core.Ingest(BrokerEvent{
        .kind = BrokerEventKind::Failure,
        .generation = ConnectionGeneration{1},
        .message = "Account stream connection failed",
    }));
    const CoreSnapshot snapshot = core.Snapshot();
    EXPECT_EQ(snapshot.safety_status, SafetyStatus::Error);
    EXPECT_FALSE(snapshot.authenticated);
    EXPECT_FALSE(snapshot.account.has_value());
}

TEST(TradingCore,
     DisconnectDuringFillRetainsVisibleStateUntilReconnectReconciliation) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));
    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Authorized, 1)));
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::TradeUpdatesAcknowledged, 1)));
    ASSERT_TRUE(core.Ingest(AccountEvent(1)));
    ASSERT_TRUE(core.Ingest(PositionsEvent(1)));

    OrderState order;
    order.id = "disconnect-fill";
    order.asset_id = "asset-1";
    order.symbol = "TEST";
    order.qty = *Decimal::Parse("1");
    order.filled_qty = Decimal::Zero();
    ASSERT_TRUE(core.Ingest(OrdersEvent(1, {order})));

    order.filled_qty = *Decimal::Parse("0.5");
    ASSERT_TRUE(core.Ingest(BrokerEvent{
        .kind = BrokerEventKind::TradeUpdate,
        .generation = ConnectionGeneration{1},
        .source_event_id = "disconnect-fill-execution",
        .payload = TradeUpdatePayload{
            .event = "partial_fill",
            .execution_id = "disconnect-fill-execution",
            .order = order,
            .position_qty = *Decimal::Parse("0.5"),
            .event_at_ms = 2000,
        },
    }));
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::Disconnected, 1)));

    const CoreSnapshot stale = core.Snapshot();
    EXPECT_EQ(stale.safety_status, SafetyStatus::Stale);
    ASSERT_EQ(stale.orders.size(), 1U);
    EXPECT_EQ(stale.orders.front().filled_qty.ToString(), "0.5");
    ASSERT_EQ(stale.positions.size(), 1U);
    EXPECT_EQ(stale.positions.front().qty.ToString(), "0.5");
    EXPECT_FALSE(stale.trading_permitted);

    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Authorized, 1)));
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::TradeUpdatesAcknowledged, 1)));
    ASSERT_TRUE(core.Ingest(AccountEvent(1)));
    ASSERT_TRUE(core.Ingest(PositionsEvent(1)));
    ASSERT_TRUE(core.Ingest(OrdersEvent(1, {order})));
    EXPECT_EQ(core.Snapshot().safety_status, SafetyStatus::Live);
}

TEST(TradingCore, AppliesTradeUpdatesIdempotentlyByExecutionId) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));
    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Authorized, 1)));
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::TradeUpdatesAcknowledged, 1)));
    ASSERT_TRUE(core.Ingest(AccountEvent(1)));

    OrderState initial;
    initial.id = "order-1";
    initial.qty = *Decimal::Parse("1.000000001");
    initial.filled_qty = Decimal::Zero();
    initial.updated_at_ms = 10;
    ASSERT_TRUE(core.Ingest(OrdersEvent(1, {initial})));

    OrderState filled = initial;
    filled.filled_qty = *Decimal::Parse("0.000000001");
    filled.updated_at_ms = 20;
    TradeUpdatePayload update{
        .event = "partial_fill",
        .execution_id = "execution-1",
        .order = filled,
        .fill_qty = *Decimal::Parse("0.000000001"),
        .fill_price = *Decimal::Parse("123.456789"),
        .event_at_ms = 2000,
    };
    BrokerEvent broker_event{
        .kind = BrokerEventKind::TradeUpdate,
        .generation = ConnectionGeneration{1},
        .source_event_id = "execution-1",
        .payload = update,
    };
    ASSERT_TRUE(core.Ingest(broker_event));
    ASSERT_TRUE(core.Ingest(std::move(broker_event)));

    const CoreSnapshot snapshot = core.Snapshot();
    ASSERT_EQ(snapshot.orders.size(), 1U);
    EXPECT_EQ(snapshot.orders.front().filled_qty.ToString(),
              "0.000000001");
    EXPECT_EQ(snapshot.orders.front().last_event, "partial_fill");
}

TEST(TradingCore, BuffersTradeUpdatesUntilInitialOrderSnapshot) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));

    OrderState updated;
    updated.id = "order-buffered";
    updated.qty = *Decimal::Parse("2");
    updated.filled_qty = *Decimal::Parse("1");
    updated.updated_at_ms = 20;
    ASSERT_TRUE(core.Ingest(BrokerEvent{
        .kind = BrokerEventKind::TradeUpdate,
        .generation = ConnectionGeneration{1},
        .source_event_id = "execution-buffered",
        .payload = TradeUpdatePayload{
            .event = "partial_fill",
            .execution_id = "execution-buffered",
            .order = updated,
            .event_at_ms = 2000,
        },
    }));

    OrderState stale = updated;
    stale.filled_qty = Decimal::Zero();
    stale.updated_at_ms = 10;
    ASSERT_TRUE(core.Ingest(OrdersEvent(1, {stale})));

    const CoreSnapshot snapshot = core.Snapshot();
    ASSERT_EQ(snapshot.orders.size(), 1U);
    EXPECT_EQ(snapshot.orders.front().filled_qty.ToString(), "1");
}

TEST(TradingCore, QuarantinesImpossibleFilledQuantity) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));
    ASSERT_TRUE(core.Ingest(AccountEvent(1)));

    OrderState initial;
    initial.id = "order-1";
    initial.qty = *Decimal::Parse("1");
    initial.filled_qty = Decimal::Zero();
    ASSERT_TRUE(core.Ingest(OrdersEvent(1, {initial})));

    OrderState impossible = initial;
    impossible.filled_qty = *Decimal::Parse("1.000000001");
    const auto result = core.Ingest(BrokerEvent{
        .kind = BrokerEventKind::TradeUpdate,
        .generation = ConnectionGeneration{1},
        .source_event_id = "execution-impossible",
        .payload = TradeUpdatePayload{
            .event = "fill",
            .execution_id = "execution-impossible",
            .order = impossible,
        },
    });

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, CoreErrorCode::InvalidTransition);
    EXPECT_EQ(core.Snapshot().safety_status, SafetyStatus::Reconciling);
    ASSERT_EQ(core.Snapshot().orders.size(), 1U);
    EXPECT_TRUE(core.Snapshot().orders.front().filled_qty.IsZero());
}

TEST(TradingCore, AccountRestrictionsPreventTradingEvenWhenStateIsLive) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));
    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Authorized, 1)));
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::TradeUpdatesAcknowledged, 1)));

    BrokerEvent restricted = AccountEvent(1);
    auto& account =
        std::get<AccountSnapshotPayload>(restricted.payload).account;
    account.trading_blocked = true;
    ASSERT_TRUE(core.Ingest(std::move(restricted)));
    ASSERT_TRUE(core.Ingest(PositionsEvent(1)));
    ASSERT_TRUE(core.Ingest(OrdersEvent(1)));

    const CoreSnapshot snapshot = core.Snapshot();
    EXPECT_EQ(snapshot.safety_status, SafetyStatus::Live);
    EXPECT_FALSE(snapshot.trading_permitted);
}

TEST(TradingCore, TradeCorrectionRequiresRestReconciliation) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));
    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Authorized, 1)));
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::TradeUpdatesAcknowledged, 1)));
    ASSERT_TRUE(core.Ingest(AccountEvent(1)));
    ASSERT_TRUE(core.Ingest(PositionsEvent(1)));

    OrderState order;
    order.id = "corrected-order";
    order.qty = *Decimal::Parse("1");
    order.filled_qty = *Decimal::Parse("1");
    ASSERT_TRUE(core.Ingest(OrdersEvent(1, {order})));
    ASSERT_EQ(core.Snapshot().safety_status, SafetyStatus::Live);

    ASSERT_TRUE(core.Ingest(BrokerEvent{
        .kind = BrokerEventKind::TradeUpdate,
        .generation = ConnectionGeneration{1},
        .source_event_id = "correction-1",
        .payload = TradeUpdatePayload{
            .event = "trade_correct",
            .execution_id = "execution-1",
            .order = order,
            .event_at_ms = 2000,
        },
    }));

    const CoreSnapshot snapshot = core.Snapshot();
    EXPECT_EQ(snapshot.safety_status, SafetyStatus::Reconciling);
    EXPECT_FALSE(snapshot.reconciled);
}

TEST(TradingCore, FillPublishesProvisionalExactPositionUntilRestSnapshots) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));
    ASSERT_TRUE(core.Ingest(Event(BrokerEventKind::Authorized, 1)));
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::TradeUpdatesAcknowledged, 1)));
    ASSERT_TRUE(core.Ingest(AccountEvent(1)));
    ASSERT_TRUE(core.Ingest(PositionsEvent(1)));

    OrderState order;
    order.id = "fractional-fill";
    order.asset_id = "asset-1";
    order.symbol = "TEST";
    order.qty = *Decimal::Parse("1");
    order.filled_qty = Decimal::Zero();
    ASSERT_TRUE(core.Ingest(OrdersEvent(1, {order})));

    order.filled_qty = *Decimal::Parse("0.123456789");
    ASSERT_TRUE(core.Ingest(BrokerEvent{
        .kind = BrokerEventKind::TradeUpdate,
        .generation = ConnectionGeneration{1},
        .source_event_id = "execution-fractional",
        .payload = TradeUpdatePayload{
            .event = "partial_fill",
            .execution_id = "execution-fractional",
            .order = order,
            .position_qty = *Decimal::Parse("0.123456789"),
            .event_at_ms = 2000,
        },
    }));

    const CoreSnapshot provisional = core.Snapshot();
    ASSERT_EQ(provisional.positions.size(), 1U);
    EXPECT_EQ(provisional.positions.front().qty.ToString(),
              "0.123456789");
    EXPECT_TRUE(provisional.positions.front().provisional);
    EXPECT_FALSE(provisional.positions_snapshot_loaded);
    EXPECT_FALSE(provisional.account_snapshot_loaded);
    EXPECT_EQ(provisional.safety_status, SafetyStatus::Reconciling);

    PositionState confirmed;
    confirmed.asset_id = "asset-1";
    confirmed.symbol = "TEST";
    confirmed.qty = *Decimal::Parse("0.123456789");
    ASSERT_TRUE(core.Ingest(PositionsEvent(1, {confirmed})));
    ASSERT_TRUE(core.Ingest(AccountEvent(1)));

    const CoreSnapshot reconciled = core.Snapshot();
    ASSERT_EQ(reconciled.positions.size(), 1U);
    EXPECT_FALSE(reconciled.positions.front().provisional);
    EXPECT_EQ(reconciled.safety_status, SafetyStatus::Live);
}

TEST(TradingCore, ProjectsLongPositionFromCanonicalMarketPriceExactly) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    PositionState position;
    position.asset_id = "asset-1";
    position.symbol = "TEST";
    position.side = "long";
    position.qty = D("2.5");
    position.avg_entry_price = D("100");
    position.lastday_price = D("99.5");
    ASSERT_TRUE(core.Ingest(PositionsEvent(0, {position})));

    core.ApplyMarketData(MarketDataSnapshot{
        .instrument_id = "asset-1",
        .symbol = "TEST",
        .feed = MarketDataFeed::Iex,
        .stream_status = MarketStreamStatus::Subscribed,
        .trades_subscribed = true,
        .latest_price = CanonicalMarketPrice{
            .price = D("101.2"),
            .event_time_ns = 10,
            .received_at_ms = 2000,
        },
    });

    const CoreSnapshot snapshot = core.Snapshot();
    const PositionState& valued = snapshot.positions.front();
    EXPECT_TRUE(valued.valuation_current);
    EXPECT_TRUE(valued.valuation_from_market_stream);
    EXPECT_EQ(valued.valuation_feed, MarketDataFeed::Iex);
    EXPECT_EQ(valued.current_price.ToString(), "101.2");
    EXPECT_EQ(valued.market_value.ToString(), "253");
    EXPECT_EQ(valued.unrealized_pl.ToString(), "3");
    EXPECT_EQ(valued.unrealized_plpc.ToString(), "0.012");
    EXPECT_EQ(valued.unrealized_intraday_pl.ToString(), "4.25");
    EXPECT_EQ(valued.change_today.ToString(), "0.017085427");
}

TEST(TradingCore, ProjectsShortPositionWithSignedExactPnL) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    PositionState position;
    position.asset_id = "asset-short";
    position.symbol = "SHORT";
    position.side = "short";
    position.qty = D("-3");
    position.avg_entry_price = D("100");
    position.lastday_price = D("101");
    ASSERT_TRUE(core.Ingest(PositionsEvent(0, {position})));

    core.ApplyMarketData(MarketDataSnapshot{
        .instrument_id = "asset-short",
        .symbol = "SHORT",
        .feed = MarketDataFeed::Sip,
        .stream_status = MarketStreamStatus::Subscribed,
        .trades_subscribed = true,
        .latest_price = CanonicalMarketPrice{
            .price = D("95"),
            .event_time_ns = 30,
            .received_at_ms = 2000,
        },
    });

    const CoreSnapshot snapshot = core.Snapshot();
    const PositionState& valued = snapshot.positions.front();
    EXPECT_EQ(valued.market_value.ToString(), "-285");
    EXPECT_EQ(valued.unrealized_pl.ToString(), "15");
    EXPECT_EQ(valued.unrealized_plpc.ToString(), "0.05");
    EXPECT_EQ(valued.unrealized_intraday_pl.ToString(), "18");
    EXPECT_EQ(valued.unrealized_intraday_plpc.ToString(),
              "0.059405941");
    EXPECT_EQ(valued.change_today.ToString(), "-0.059405941");
}

TEST(TradingCore,
     CorrectionsRevalueWhileWrongFeedAndStalePricesAreNotCurrent) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    PositionState position;
    position.asset_id = "asset-1";
    position.symbol = "TEST";
    position.qty = D("1");
    position.avg_entry_price = D("100");
    position.lastday_price = D("99");
    ASSERT_TRUE(core.Ingest(PositionsEvent(0, {position})));

    auto market = MarketDataSnapshot{
        .instrument_id = "asset-1",
        .symbol = "TEST",
        .feed = MarketDataFeed::Iex,
        .stream_status = MarketStreamStatus::Subscribed,
        .trades_subscribed = true,
        .latest_price = CanonicalMarketPrice{
            .price = D("102"),
            .event_time_ns = 50,
            .received_at_ms = 2000,
        },
    };
    core.ApplyMarketData(market);
    market.latest_price->price = D("101");
    core.ApplyMarketData(market);
    EXPECT_EQ(core.Snapshot().positions.front()
                  .current_price.ToString(),
              "101");

    market.feed = MarketDataFeed::Sip;
    market.latest_price->price = D("999");
    market.latest_price->event_time_ns = 51;
    core.ApplyMarketData(market);
    auto snapshot = core.Snapshot();
    EXPECT_FALSE(snapshot.positions.front().valuation_current);
    EXPECT_EQ(snapshot.positions.front().current_price.ToString(),
              "101");

    market.feed = MarketDataFeed::Iex;
    market.stream_status = MarketStreamStatus::Stale;
    market.latest_price->price = D("103");
    market.latest_price->event_time_ns = 52;
    core.ApplyMarketData(market);
    snapshot = core.Snapshot();
    EXPECT_FALSE(snapshot.positions.front().valuation_current);
    EXPECT_EQ(snapshot.positions.front().current_price.ToString(),
              "101");

    market.feed = MarketDataFeed::Sip;
    market.stream_status = MarketStreamStatus::Connecting;
    market.latest_price.reset();
    core.ApplyMarketData(market);
    market.stream_status = MarketStreamStatus::Subscribed;
    market.latest_price = CanonicalMarketPrice{
        .price = D("104"),
        .event_time_ns = 53,
        .received_at_ms = 2003,
    };
    core.ApplyMarketData(market);
    snapshot = core.Snapshot();
    EXPECT_TRUE(snapshot.positions.front().valuation_current);
    EXPECT_EQ(snapshot.positions.front().valuation_feed,
              MarketDataFeed::Sip);
    EXPECT_EQ(snapshot.positions.front().current_price.ToString(),
              "104");
}

TEST(TradingCore,
     RestSnapshotReanchorsValuationAndPartialFillDoesNotInventBasis) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    PositionState authoritative;
    authoritative.asset_id = "asset-1";
    authoritative.symbol = "TEST";
    authoritative.qty = D("4");
    authoritative.avg_entry_price = D("50");
    authoritative.current_price = D("51");
    authoritative.market_value = D("204");
    authoritative.unrealized_pl = D("4");
    ASSERT_TRUE(
        core.Ingest(PositionsEvent(0, {authoritative})));
    auto snapshot = core.Snapshot();
    EXPECT_TRUE(snapshot.positions.front().valuation_current);
    EXPECT_FALSE(snapshot.positions.front()
                     .valuation_from_market_stream);
    EXPECT_EQ(snapshot.positions.front().market_value.ToString(),
              "204");
    core.ApplyMarketData(MarketDataSnapshot{
        .instrument_id = "asset-1",
        .symbol = "TEST",
        .feed = MarketDataFeed::Iex,
        .stream_status = MarketStreamStatus::Subscribed,
        .trades_subscribed = true,
        .latest_price = CanonicalMarketPrice{
            .price = D("49"),
            .event_time_ns = 1,
            .received_at_ms = 1000,
        },
    });
    snapshot = core.Snapshot();
    EXPECT_FALSE(snapshot.positions.front()
                     .valuation_from_market_stream);
    EXPECT_EQ(snapshot.positions.front().current_price.ToString(),
              "51");

    PositionState provisional;
    provisional.asset_id = "asset-2";
    provisional.symbol = "NEW";
    provisional.qty = D("0.25");
    provisional.provisional = true;
    ASSERT_TRUE(core.Ingest(BrokerEvent{
        .kind = BrokerEventKind::PositionsSnapshot,
        .generation = ConnectionGeneration{0},
        .payload = PositionsSnapshotPayload{
            .positions = {provisional},
            .received_at_ms = 0,
        },
    }));
    core.ApplyMarketData(MarketDataSnapshot{
        .instrument_id = "asset-2",
        .symbol = "NEW",
        .feed = MarketDataFeed::Iex,
        .stream_status = MarketStreamStatus::Subscribed,
        .trades_subscribed = true,
        .latest_price = CanonicalMarketPrice{
            .price = D("20"),
            .event_time_ns = 70,
            .received_at_ms = 80,
        },
    });
    snapshot = core.Snapshot();
    EXPECT_FALSE(snapshot.positions.front().valuation_current);
    EXPECT_EQ(snapshot.positions.front().market_value.ToString(),
              "0");
}

TEST(TradingCore, CommandsAreTypedAndReceiveMonotonicIds) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);

    const auto connect =
        core.Submit(ConnectAccount{AccountEnvironment::Live});
    const auto disconnect = core.Submit(DisconnectAccount{});

    ASSERT_TRUE(connect);
    ASSERT_TRUE(disconnect);
    EXPECT_TRUE(connect->accepted);
    EXPECT_EQ(connect->command_id, 1U);
    EXPECT_EQ(disconnect->command_id, 2U);
    EXPECT_EQ(core.Snapshot().environment, AccountEnvironment::Live);
    EXPECT_EQ(core.Snapshot().safety_status, SafetyStatus::Disconnected);
}

TEST(TradingCore, JournalOrderMatchesConcurrentReductionOrder) {
    MemoryJournal journal;
    FixedClock clock;
    TradingCore core(journal, clock);
    ASSERT_TRUE(core.Ingest(
        Event(BrokerEventKind::ConnectionAttemptStarted, 1)));

    std::barrier start{3};
    auto ingest_account = [&core, &start](std::string id) {
        AccountState account;
        account.id = std::move(id);
        account.status = "ACTIVE";
        start.arrive_and_wait();
        EXPECT_TRUE(core.Ingest(BrokerEvent{
            .kind = BrokerEventKind::AccountSnapshot,
            .generation = ConnectionGeneration{1},
            .payload =
                AccountSnapshotPayload{.account = std::move(account)},
        }));
    };
    std::thread first(ingest_account, "account-a");
    std::thread second(ingest_account, "account-b");
    start.arrive_and_wait();
    first.join();
    second.join();

    ASSERT_TRUE(core.Snapshot().account);
    ASSERT_GE(journal.events.size(), 3U);
    const auto& last_payload =
        std::get<AccountSnapshotPayload>(journal.events.back().payload);
    EXPECT_EQ(core.Snapshot().account->id, last_payload.account.id);
}

}  // namespace
