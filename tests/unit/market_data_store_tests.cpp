#include "tradebox/core/market_data_store.h"

#include <gtest/gtest.h>

#include <tuple>
#include <vector>

namespace {

using namespace tradebox::core;

Decimal D(const char* value) {
    return *Decimal::Parse(value);
}

MarketTrade Trade(const char* id, const char* price,
                  const char* timestamp,
                  std::int64_t event_time_ns = 0) {
    return {
        .symbol = "AAPL",
        .trade_id = id,
        .price = D(price),
        .size = D("100"),
        .broker_timestamp = timestamp,
        .event_time_ns = event_time_ns,
        .received_at_ms = 1000,
    };
}

TEST(MarketDataStore, PublishesExactQuoteAndSubscriptionHealth) {
    MarketDataStore store;
    store.Ingest(MarketStreamChanged{
        .status = MarketStreamStatus::Subscribed,
        .feed = MarketDataFeed::Iex,
        .trade_symbols = {"AAPL"},
        .quote_symbols = {"AAPL"},
        .message = "subscribed",
        .received_at_ms = 500,
    });
    store.Ingest(QuoteReceived{
        .quote =
            {
                .symbol = "AAPL",
                .bid_price = D("201.1234"),
                .bid_size = D("2"),
                .ask_price = D("201.1235"),
                .ask_size = D("3"),
                .broker_timestamp =
                    "2026-07-28T12:00:00.000000001Z",
                .received_at_ms = 1000,
            },
    });

    const auto snapshot = store.Snapshot("AAPL");
    EXPECT_EQ(snapshot.feed, MarketDataFeed::Iex);
    EXPECT_EQ(snapshot.stream_status, MarketStreamStatus::Subscribed);
    EXPECT_TRUE(snapshot.trades_subscribed);
    EXPECT_TRUE(snapshot.quotes_subscribed);
    EXPECT_EQ(snapshot.projection_started_at_ns, 500'000'000);
    ASSERT_TRUE(snapshot.latest_quote);
    EXPECT_EQ(snapshot.latest_quote->bid_price.ToString(), "201.1234");
    EXPECT_EQ(snapshot.latest_quote->ask_price.ToString(), "201.1235");
}

TEST(MarketDataStore, IgnoresOutOfOrderQuote) {
    MarketDataStore store;
    store.Ingest(QuoteReceived{.quote = {
        .symbol = "AAPL",
        .bid_price = D("201"),
        .ask_price = D("202"),
        .broker_timestamp = "2026-07-28T12:00:01Z",
        .event_time_ns = 2,
    }});
    store.Ingest(QuoteReceived{.quote = {
        .symbol = "AAPL",
        .bid_price = D("199"),
        .ask_price = D("200"),
        .broker_timestamp = "2026-07-28T12:00:00Z",
        .event_time_ns = 1,
    }});

    ASSERT_TRUE(store.Snapshot("AAPL").latest_quote);
    EXPECT_EQ(store.Snapshot("AAPL").latest_quote->bid_price.ToString(),
              "201");
}

TEST(MarketDataStore, KeepsNewestTradesFirstAndBoundsTape) {
    MarketDataStore store(2);
    store.Ingest(TradeReceived{
        .trade = Trade("1", "201", "2026-07-28T12:00:00Z", 1)});
    store.Ingest(TradeReceived{
        .trade = Trade("3", "203", "2026-07-28T12:00:02Z", 3)});
    store.Ingest(TradeReceived{
        .trade = Trade("2", "202", "2026-07-28T12:00:01Z", 2)});

    const auto snapshot = store.Snapshot("AAPL");
    ASSERT_EQ(snapshot.trades.size(), 2U);
    EXPECT_EQ(snapshot.trades[0].trade_id, "3");
    EXPECT_EQ(snapshot.trades[1].trade_id, "2");
}

TEST(MarketDataStore, BatchIngestPreservesEventOrder) {
    MarketDataStore store;
    std::vector<MarketDataEventPtr> events;
    events.push_back(ShareMarketDataEvent(TradeReceived{
        .trade = Trade(
            "first", "201",
            "2026-07-28T12:00:00.000000001Z", 1),
    }));
    events.push_back(ShareMarketDataEvent(TradeReceived{
        .trade = Trade(
            "second", "202",
            "2026-07-28T12:00:00.000000002Z", 2),
    }));

    store.IngestBatch(std::move(events));

    const auto delta = store.Delta("AAPL", 0, 10);
    ASSERT_EQ(delta.events.size(), 2U);
    const auto* first =
        std::get_if<TradeReceived>(delta.events[0].event.get());
    const auto* second =
        std::get_if<TradeReceived>(delta.events[1].event.get());
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->trade.trade_id, "first");
    EXPECT_EQ(second->trade.trade_id, "second");
}

TEST(MarketDataStore, AppliesTradeCorrectionAndCancellation) {
    MarketDataStore store;
    store.Ingest(TradeReceived{
        .trade = Trade("original", "201", "2026-07-28T12:00:00Z")});
    store.Ingest(TradeCorrected{
        .symbol = "AAPL",
        .original_trade_id = "original",
        .corrected_trade =
            Trade("corrected", "201.01",
                  "2026-07-28T12:00:00.100Z"),
    });

    auto snapshot = store.Snapshot("AAPL");
    ASSERT_EQ(snapshot.trades.size(), 1U);
    EXPECT_EQ(snapshot.trades.front().trade_id, "corrected");
    EXPECT_TRUE(snapshot.trades.front().corrected);

    store.Ingest(TradeCanceled{
        .symbol = "AAPL",
        .trade_id = "corrected",
    });
    EXPECT_TRUE(store.Snapshot("AAPL").trades.empty());
}

TEST(MarketDataStore, DeduplicatesTradeIds) {
    MarketDataStore store;
    store.Ingest(TradeReceived{
        .trade = Trade("same", "201", "2026-07-28T12:00:00Z")});
    store.Ingest(TradeReceived{
        .trade = Trade("same", "999", "2026-07-28T12:00:01Z")});

    const auto snapshot = store.Snapshot("AAPL");
    ASSERT_EQ(snapshot.trades.size(), 1U);
    EXPECT_EQ(snapshot.trades.front().price.ToString(), "201");
}

TEST(MarketDataStore, AllowsBrokerTradeIdsToRepeatOnLaterDays) {
    constexpr std::int64_t day_ns =
        24LL * 60 * 60 * 1'000'000'000;
    MarketDataStore store;
    store.Ingest(TradeReceived{
        .trade = Trade("same", "201", "day-one", 1)});
    store.Ingest(TradeReceived{
        .trade = Trade("same", "202", "day-two", day_ns + 1)});

    const auto snapshot = store.Snapshot("AAPL");
    ASSERT_EQ(snapshot.trades.size(), 2U);
    EXPECT_EQ(snapshot.trades.front().price.ToString(), "202");
}

TEST(MarketDataStore, ReadsOnlySequencedChangesAfterCursor) {
    MarketDataStore store;
    store.Ingest(TradeReceived{
        .trade = Trade("1", "201", "2026-07-28T12:00:00Z", 1)});
    const auto first = store.Delta("AAPL", 0, 10);
    ASSERT_EQ(first.events.size(), 1U);
    EXPECT_EQ(first.events.front().sequence, 1U);

    store.Ingest(QuoteReceived{.quote = {
        .symbol = "AAPL",
        .bid_price = D("201.01"),
        .ask_price = D("201.02"),
        .event_time_ns = 2,
    }});
    store.Ingest(TradeReceived{
        .trade = Trade("2", "202", "2026-07-28T12:00:01Z", 3)});

    const auto second =
        store.Delta("AAPL", first.next_sequence, 10);
    ASSERT_EQ(second.events.size(), 2U);
    EXPECT_EQ(second.events.front().sequence, 2U);
    EXPECT_EQ(second.events.back().sequence, 3U);
    const auto* quote = std::get_if<QuoteReceived>(
        second.events.front().event.get());
    ASSERT_NE(quote, nullptr);
    EXPECT_EQ(quote->quote.bid_price.ToString(), "201.01");
    EXPECT_FALSE(second.latest_quote);
    EXPECT_FALSE(second.gap_detected);

    const auto unchanged =
        store.Delta("AAPL", second.next_sequence, 10);
    EXPECT_TRUE(unchanged.events.empty());
    EXPECT_FALSE(unchanged.latest_quote);
}

TEST(MarketDataStore, UsesStableInstrumentIdentityAndRetainsSymbolAlias) {
    MarketDataStore store;
    store.Ingest(TradeReceived{
        .trade = Trade("1", "201", "first", 1)});

    MarketTrade identified =
        Trade("2", "202", "identified", 2);
    identified.instrument_id = "instrument:apple-common";
    store.Ingest(TradeReceived{
        .trade = std::move(identified)});

    const auto by_symbol = store.Snapshot("AAPL");
    const auto by_identity =
        store.Snapshot("instrument:apple-common");
    EXPECT_EQ(by_symbol.instrument_id,
              "instrument:apple-common");
    EXPECT_EQ(by_symbol.symbol, "AAPL");
    ASSERT_EQ(by_symbol.trades.size(), 2U);
    ASSERT_EQ(by_identity.trades.size(), 2U);
    EXPECT_EQ(by_identity.trades.front().price.ToString(), "202");
}

TEST(MarketDataStore, ReportsOnlyChangedInstrumentsAfterCursor) {
    MarketDataStore store;
    auto apple = Trade("1", "201", "apple", 1);
    apple.instrument_id = "instrument:apple";
    store.Ingest(TradeReceived{.trade = std::move(apple)});

    const auto first = store.Changes(0, 10);
    ASSERT_EQ(first.instruments.size(), 1U);
    EXPECT_EQ(first.instruments.front().instrument_id,
              "instrument:apple");
    EXPECT_EQ(first.instruments.front().symbol, "AAPL");

    EXPECT_TRUE(
        store.Changes(first.next_sequence, 10)
            .instruments.empty());

    MarketQuote quote{
        .instrument_id = "instrument:microsoft",
        .symbol = "MSFT",
        .bid_price = D("500.01"),
        .ask_price = D("500.02"),
        .event_time_ns = 2,
    };
    store.Ingest(QuoteReceived{.quote = std::move(quote)});
    const auto second =
        store.Changes(first.next_sequence, 10);
    ASSERT_EQ(second.instruments.size(), 1U);
    EXPECT_EQ(second.instruments.front().symbol, "MSFT");
}

TEST(MarketDataStore, ReportsGapWhenConsumerFallsBehindBoundedRing) {
    MarketDataStore store(2);
    store.Ingest(TradeReceived{
        .trade = Trade("1", "201", "2026-07-28T12:00:00Z", 1)});
    store.Ingest(TradeReceived{
        .trade = Trade("2", "202", "2026-07-28T12:00:01Z", 2)});
    store.Ingest(TradeReceived{
        .trade = Trade("3", "203", "2026-07-28T12:00:02Z", 3)});
    store.Ingest(TradeReceived{
        .trade = Trade("4", "204", "2026-07-28T12:00:03Z", 4)});

    const auto delta = store.Delta("AAPL", 1, 10);
    EXPECT_TRUE(delta.gap_detected);
    ASSERT_EQ(delta.events.size(), 2U);
    EXPECT_EQ(delta.events.front().sequence, 3U);
    EXPECT_EQ(delta.events.back().sequence, 4U);
}

TEST(MarketDataStore,
     SharesOneImmutableEventAcrossTheDeltaRing) {
    MarketDataStore store;
    const auto event = ShareMarketDataEvent(TradeReceived{
        .trade = Trade("shared", "201", "shared", 1),
    });
    store.Ingest(event);

    const auto delta = store.Delta("AAPL", 0, 10);
    ASSERT_EQ(delta.events.size(), 1U);
    EXPECT_EQ(delta.events[0].event.get(), event.get());
}

TEST(MarketDataStore,
     ChangedInstrumentRingReportsConsumerOverrun) {
    MarketDataStore store(2, 2);
    for (int index = 0; index < 4; ++index) {
        MarketTrade trade =
            Trade(("id-" + std::to_string(index)).c_str(),
                  "201", "change", index + 1);
        trade.symbol =
            "S" + std::to_string(index);
        trade.instrument_id =
            "instrument:" + std::to_string(index);
        store.Ingest(TradeReceived{
            .trade = std::move(trade),
        });
    }

    const auto changes = store.Changes(1, 10);
    EXPECT_TRUE(changes.gap_detected);
    ASSERT_EQ(changes.instruments.size(), 2U);
    EXPECT_EQ(changes.instruments.front().sequence, 3U);
    EXPECT_EQ(changes.instruments.back().sequence, 4U);
}

TEST(MarketDataStore,
     PublishesCanonicalPriceAndProvisionalMinuteAtomically) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    MarketDataStore store;
    store.Ingest(MarketStreamChanged{
        .status = MarketStreamStatus::Subscribed,
        .feed = MarketDataFeed::Sip,
        .trade_symbols = {"AAPL"},
    });
    auto first = Trade("1", "201.25", "first", minute + 10);
    first.tape = "C";
    first.conditions = {"@"};
    first.size = D("100.5");
    store.Ingest(TradeReceived{.trade = std::move(first)});
    auto second = Trade("2", "202.75", "second", minute + 20);
    second.tape = "C";
    second.conditions = {"@"};
    second.size = D("2.25");
    store.Ingest(TradeReceived{.trade = std::move(second)});

    const auto snapshot = store.Snapshot("AAPL");
    ASSERT_TRUE(snapshot.latest_price);
    EXPECT_EQ(snapshot.latest_price->price.ToString(), "202.75");
    EXPECT_EQ(snapshot.latest_price->trade_id, "2");
    ASSERT_EQ(snapshot.provisional_minute_bars.size(), 1U);
    const auto& bar = snapshot.provisional_minute_bars.front();
    EXPECT_EQ(bar.start_ns, minute);
    EXPECT_EQ(bar.open.ToString(), "201.25");
    EXPECT_EQ(bar.high.ToString(), "202.75");
    EXPECT_EQ(bar.low.ToString(), "201.25");
    EXPECT_EQ(bar.close.ToString(), "202.75");
    EXPECT_EQ(bar.volume.ToString(), "102.75");
    EXPECT_EQ(bar.trade_count, 2U);
}

TEST(MarketDataStore,
     PublishesPressureAlongsideLatestPriceAndDelta) {
    MarketDataStore store;
    store.Ingest(QuoteReceived{.quote = {
        .symbol = "AAPL",
        .bid_price = D("99"),
        .ask_price = D("101"),
        .event_time_ns = 1'000'000'000,
        .received_at_ms = 1'000,
    }});
    auto trade = Trade("buyer", "101", "100", 1'001'000'000);
    trade.tape = "C";
    trade.conditions = {"@"};
    trade.received_at_ms = 1'001;
    store.Ingest(TradeReceived{.trade = std::move(trade)});

    const auto snapshot = store.Snapshot("AAPL");
    ASSERT_TRUE(snapshot.latest_price);
    EXPECT_EQ(snapshot.latest_price->price.ToString(), "101");
    ASSERT_TRUE(snapshot.trade_pressure);
    EXPECT_EQ(snapshot.trade_pressure->buyer_trades, 1U);
    EXPECT_EQ(snapshot.trade_pressure->seller_trades, 0U);
    EXPECT_EQ(snapshot.trade_pressure->latest_method,
              TradeClassificationMethod::QuoteTest);

    const auto delta = store.Delta("AAPL", 0, 10);
    ASSERT_TRUE(delta.trade_pressure);
    EXPECT_EQ(delta.trade_pressure->buyer_trades, 1U);
    EXPECT_EQ(delta.events.size(), 2U);
}

TEST(MarketDataStore,
     BatchIngestPublishesPressureInEventOrder) {
    MarketDataStore store;
    auto quote = QuoteReceived{.quote = {
        .symbol = "AAPL",
        .bid_price = D("99"),
        .ask_price = D("101"),
        .event_time_ns = 1'000'000'000,
        .received_at_ms = 1'000,
    }};
    auto trade = Trade("buyer", "101", "100", 1'001'000'000);
    trade.tape = "C";
    trade.conditions = {"@"};
    trade.received_at_ms = 1'001;
    store.IngestBatch({
        ShareMarketDataEvent(std::move(quote)),
        ShareMarketDataEvent(TradeReceived{.trade = std::move(trade)}),
    });

    const auto snapshot = store.Snapshot("AAPL");
    ASSERT_TRUE(snapshot.trade_pressure);
    EXPECT_EQ(snapshot.trade_pressure->buyer_trades, 1U);
    EXPECT_EQ(snapshot.trade_pressure->latest_method,
              TradeClassificationMethod::QuoteTest);
}

TEST(MarketDataStore,
     DuplicateTradeDoesNotAddDuplicatePressureOrChangedInstrument) {
    MarketDataStore store;
    auto trade = Trade("same", "101", "100", 1'001'000'000);
    trade.received_at_ms = 1'001;
    store.Ingest(TradeReceived{.trade = trade});
    const auto first = store.Changes(0, 10);
    ASSERT_EQ(first.instruments.size(), 1U);

    store.Ingest(TradeReceived{.trade = std::move(trade)});
    const auto snapshot = store.Snapshot("AAPL");
    ASSERT_TRUE(snapshot.trade_pressure);
    EXPECT_EQ(snapshot.trade_pressure->buyer_trades, 0U);
    EXPECT_EQ(snapshot.trade_pressure->unknown_trades, 1U);
    EXPECT_TRUE(
        store.Changes(first.next_sequence, 10).instruments.empty());
}

TEST(MarketDataStore,
     CorrectionAndCancellationUpdatePressureWithCanonicalPrice) {
    MarketDataStore store;
    store.Ingest(QuoteReceived{.quote = {
        .symbol = "AAPL",
        .bid_price = D("99"),
        .ask_price = D("101"),
        .event_time_ns = 1'000'000'000,
        .received_at_ms = 1'000,
    }});
    auto original = Trade("original", "101", "100", 1'001'000'000);
    original.tape = "C";
    original.conditions = {"@"};
    original.received_at_ms = 1'001;
    store.Ingest(TradeReceived{.trade = std::move(original)});

    auto corrected = Trade("corrected", "99", "100", 1'001'000'000);
    corrected.tape = "C";
    corrected.conditions = {"@"};
    corrected.received_at_ms = 1'002;
    store.Ingest(TradeCorrected{
        .symbol = "AAPL",
        .original_trade_id = "original",
        .corrected_trade = std::move(corrected),
    });
    auto snapshot = store.Snapshot("AAPL");
    ASSERT_TRUE(snapshot.latest_price);
    EXPECT_EQ(snapshot.latest_price->price.ToString(), "99");
    ASSERT_TRUE(snapshot.trade_pressure);
    EXPECT_EQ(snapshot.trade_pressure->buyer_trades, 0U);
    EXPECT_EQ(snapshot.trade_pressure->seller_trades, 1U);

    store.Ingest(TradeCanceled{
        .symbol = "AAPL",
        .trade_id = "corrected",
        .event_time_ns = 1'001'000'000,
        .received_at_ms = 1'003,
    });
    snapshot = store.Snapshot("AAPL");
    EXPECT_FALSE(snapshot.latest_price);
    ASSERT_TRUE(snapshot.trade_pressure);
    EXPECT_EQ(snapshot.trade_pressure->buyer_trades, 0U);
    EXPECT_EQ(snapshot.trade_pressure->seller_trades, 0U);
    EXPECT_EQ(snapshot.trade_pressure->activity, 0.0);
}

TEST(MarketDataStore,
     ReconnectMarksPressureStaleAndNewEventsResumeIt) {
    MarketDataStore store;
    store.Ingest(MarketStreamChanged{
        .status = MarketStreamStatus::Subscribed,
        .feed = MarketDataFeed::Iex,
        .trade_symbols = {"AAPL"},
        .quote_symbols = {"AAPL"},
    });
    auto trade = Trade("before", "101", "100", 1'001'000'000);
    trade.received_at_ms = 1'001;
    store.Ingest(TradeReceived{.trade = std::move(trade)});
    ASSERT_TRUE(store.Snapshot("AAPL").trade_pressure);
    EXPECT_FALSE(store.Snapshot("AAPL").trade_pressure->stale);

    store.Ingest(MarketStreamChanged{
        .status = MarketStreamStatus::Connecting,
        .feed = MarketDataFeed::Iex,
        .received_at_ms = 1'002,
    });
    ASSERT_TRUE(store.Snapshot("AAPL").trade_pressure);
    EXPECT_TRUE(store.Snapshot("AAPL").trade_pressure->stale);

    auto resumed = Trade("after", "101", "100", 1'003'000'000);
    resumed.received_at_ms = 1'003;
    store.Ingest(TradeReceived{.trade = std::move(resumed)});
    ASSERT_TRUE(store.Snapshot("AAPL").trade_pressure);
    EXPECT_FALSE(store.Snapshot("AAPL").trade_pressure->stale);

    store.Ingest(MarketStreamChanged{
        .status = MarketStreamStatus::Subscribed,
        .feed = MarketDataFeed::Sip,
    });
    ASSERT_TRUE(store.Snapshot("AAPL").trade_pressure);
    EXPECT_TRUE(store.Snapshot("AAPL").trade_pressure->stale);
}

TEST(MarketDataStore,
     PressureChangesUseTheExistingChangedInstrumentCursor) {
    MarketDataStore store;
    auto first_trade = Trade("first", "100", "100", 1'000'000'000);
    first_trade.received_at_ms = 1'000;
    store.Ingest(TradeReceived{.trade = std::move(first_trade)});
    const auto first = store.Changes(0, 10);
    ASSERT_EQ(first.instruments.size(), 1U);

    store.Ingest(QuoteReceived{.quote = {
        .symbol = "AAPL",
        .bid_price = D("99"),
        .ask_price = D("101"),
        .event_time_ns = 1'001'000'000,
        .received_at_ms = 1'001,
    }});
    const auto second =
        store.Changes(first.next_sequence, 10);
    ASSERT_EQ(second.instruments.size(), 1U);
    EXPECT_EQ(second.instruments.front().symbol, "AAPL");
}

TEST(MarketDataStore,
     AppliesAlpacaMinuteConditionRulesToLiveProjection) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    MarketDataStore store;
    auto regular = Trade("regular", "200", "regular", minute + 1);
    regular.tape = "C";
    regular.conditions = {"@"};
    regular.size = D("10");
    store.Ingest(TradeReceived{.trade = std::move(regular)});

    auto odd_lot = Trade("odd", "999", "odd", minute + 2);
    odd_lot.tape = "C";
    odd_lot.conditions = {"@", "I"};
    odd_lot.size = D("3");
    store.Ingest(TradeReceived{.trade = std::move(odd_lot)});

    const auto snapshot = store.Snapshot("AAPL");
    ASSERT_TRUE(snapshot.latest_price);
    EXPECT_EQ(snapshot.latest_price->price.ToString(), "200");
    ASSERT_EQ(snapshot.provisional_minute_bars.size(), 1U);
    EXPECT_EQ(snapshot.provisional_minute_bars[0].high.ToString(),
              "200");
    EXPECT_EQ(snapshot.provisional_minute_bars[0].close.ToString(),
              "200");
    EXPECT_EQ(snapshot.provisional_minute_bars[0].volume.ToString(),
              "13");
    EXPECT_EQ(snapshot.provisional_minute_bars[0].trade_count, 2U);
}

TEST(MarketDataStore,
     RebuildsLivePriceAndMinuteAfterCorrectionAndCancel) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    MarketDataStore store;
    auto first = Trade("first", "100", "first", minute + 1);
    first.tape = "C";
    first.conditions = {"@"};
    first.size = D("10");
    store.Ingest(TradeReceived{.trade = std::move(first)});
    auto second = Trade("second", "110", "second", minute + 2);
    second.tape = "C";
    second.conditions = {"@"};
    second.size = D("20");
    store.Ingest(TradeReceived{.trade = std::move(second)});

    auto corrected =
        Trade("second-corrected", "105", "corrected", minute + 2);
    corrected.tape = "C";
    corrected.conditions = {"@"};
    corrected.size = D("5");
    store.Ingest(TradeCorrected{
        .symbol = "AAPL",
        .original_trade_id = "second",
        .corrected_trade = std::move(corrected),
    });
    auto snapshot = store.Snapshot("AAPL");
    ASSERT_TRUE(snapshot.latest_price);
    EXPECT_EQ(snapshot.latest_price->price.ToString(), "105");
    ASSERT_EQ(snapshot.provisional_minute_bars.size(), 1U);
    EXPECT_EQ(snapshot.provisional_minute_bars[0].high.ToString(),
              "105");
    EXPECT_EQ(snapshot.provisional_minute_bars[0].volume.ToString(),
              "15");

    store.Ingest(TradeCanceled{
        .symbol = "AAPL",
        .trade_id = "second-corrected",
        .event_time_ns = minute + 2,
    });
    snapshot = store.Snapshot("AAPL");
    ASSERT_TRUE(snapshot.latest_price);
    EXPECT_EQ(snapshot.latest_price->price.ToString(), "100");
    ASSERT_EQ(snapshot.provisional_minute_bars.size(), 1U);
    EXPECT_EQ(snapshot.provisional_minute_bars[0].close.ToString(),
              "100");
    EXPECT_EQ(snapshot.provisional_minute_bars[0].volume.ToString(),
              "10");
}

TEST(MarketDataStore,
     RemovesNonExtremumTradeWithoutChangingMinutePrices) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    MarketDataStore store;
    for (const auto& [id, price, size, offset] :
         std::vector<std::tuple<const char*, const char*,
                                const char*, std::int64_t>>{
             {"open", "100", "10", 1},
             {"middle", "101", "20", 2},
             {"close", "102", "30", 3},
         }) {
        auto trade = Trade(id, price, id, minute + offset);
        trade.tape = "C";
        trade.conditions = {"@"};
        trade.size = D(size);
        store.Ingest(TradeReceived{.trade = std::move(trade)});
    }

    store.Ingest(TradeCanceled{
        .symbol = "AAPL",
        .trade_id = "middle",
        .event_time_ns = minute + 2,
    });
    const auto snapshot = store.Snapshot("AAPL");
    ASSERT_TRUE(snapshot.latest_price);
    EXPECT_EQ(snapshot.latest_price->price.ToString(), "102");
    ASSERT_EQ(snapshot.provisional_minute_bars.size(), 1U);
    const auto& bar = snapshot.provisional_minute_bars[0];
    EXPECT_EQ(bar.open.ToString(), "100");
    EXPECT_EQ(bar.high.ToString(), "102");
    EXPECT_EQ(bar.low.ToString(), "100");
    EXPECT_EQ(bar.close.ToString(), "102");
    EXPECT_EQ(bar.volume.ToString(), "40");
    EXPECT_EQ(bar.trade_count, 2U);
}

TEST(MarketDataStore,
     RolloverPublishesOnlyOpenMinuteAndLateTradeStaysClosed) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    MarketDataStore store;
    auto first = Trade("first", "100", "first", minute + 1);
    first.tape = "C";
    first.conditions = {"@"};
    store.Ingest(TradeReceived{.trade = std::move(first)});

    auto next = Trade("next", "110", "next", 2 * minute + 1);
    next.tape = "C";
    next.conditions = {"@"};
    store.Ingest(TradeReceived{.trade = std::move(next)});

    auto late = Trade("late", "999", "late", minute + 2);
    late.tape = "C";
    late.conditions = {"@"};
    store.Ingest(TradeReceived{.trade = std::move(late)});

    const auto snapshot = store.Snapshot("AAPL");
    ASSERT_EQ(snapshot.provisional_minute_bars.size(), 1U);
    EXPECT_EQ(snapshot.provisional_minute_bars[0].start_ns,
              2 * minute);
    EXPECT_EQ(snapshot.provisional_minute_bars[0].high.ToString(),
              "110");
}

TEST(MarketDataStore, ReconnectBoundaryClearsLiveProjection) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    MarketDataStore store;
    store.Ingest(MarketStreamChanged{
        .status = MarketStreamStatus::Subscribed,
        .feed = MarketDataFeed::Iex,
    });
    auto trade = Trade("before", "100", "before", minute + 1);
    trade.tape = "C";
    trade.conditions = {"@"};
    store.Ingest(TradeReceived{.trade = std::move(trade)});
    ASSERT_TRUE(store.Snapshot("AAPL").latest_price);

    store.Ingest(MarketStreamChanged{
        .status = MarketStreamStatus::Connecting,
        .feed = MarketDataFeed::Iex,
    });
    const auto snapshot = store.Snapshot("AAPL");
    EXPECT_FALSE(snapshot.latest_price);
    EXPECT_TRUE(snapshot.provisional_minute_bars.empty());
}

TEST(MarketDataStore, ClearsLiveProjectionWhenFeedChanges) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    MarketDataStore store;
    store.Ingest(MarketStreamChanged{
        .status = MarketStreamStatus::Subscribed,
        .feed = MarketDataFeed::Iex,
    });
    auto trade = Trade("iex", "100", "iex", minute + 1);
    trade.tape = "C";
    trade.conditions = {"@"};
    store.Ingest(TradeReceived{.trade = std::move(trade)});
    ASSERT_TRUE(store.Snapshot("AAPL").latest_price);

    store.Ingest(MarketStreamChanged{
        .status = MarketStreamStatus::Subscribed,
        .feed = MarketDataFeed::Sip,
    });
    const auto snapshot = store.Snapshot("AAPL");
    EXPECT_FALSE(snapshot.latest_price);
    EXPECT_TRUE(snapshot.provisional_minute_bars.empty());
}

TEST(MarketDataStore,
     PublishesLatestStockTradingStatusAndIgnoresOlderStatus) {
    MarketDataStore store;
    store.Ingest(MarketStreamChanged{
        .status = MarketStreamStatus::Subscribed,
        .feed = MarketDataFeed::Sip,
        .trade_symbols = {"AAPL"},
        .quote_symbols = {"AAPL"},
        .status_symbols = {"*"},
    });
    store.Ingest(TradingStatusReceived{
        .status = {
            .instrument_id = "asset-aapl",
            .symbol = "AAPL",
            .state = SecurityTradingState::Halted,
            .status_code = "H",
            .status_message = "Trading Halt",
            .event_time_ns = 200,
            .received_at_ms = 20,
        },
    });
    store.Ingest(TradingStatusReceived{
        .status = {
            .instrument_id = "asset-aapl",
            .symbol = "AAPL",
            .state = SecurityTradingState::Trading,
            .status_code = "T",
            .status_message = "Trading Resumption",
            .event_time_ns = 100,
            .received_at_ms = 10,
        },
    });

    const auto snapshot = store.Snapshot("asset-aapl");
    EXPECT_TRUE(snapshot.statuses_subscribed);
    ASSERT_TRUE(snapshot.trading_status);
    EXPECT_EQ(snapshot.trading_status->state,
              SecurityTradingState::Halted);
    EXPECT_EQ(snapshot.trading_status->status_code, "H");
}

}  // namespace
