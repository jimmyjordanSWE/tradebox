#include "tradebox/core/market_data_store.h"

#include <gtest/gtest.h>

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

}  // namespace
