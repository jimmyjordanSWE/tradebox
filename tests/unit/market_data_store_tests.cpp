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
    ASSERT_EQ(second.events.size(), 1U);
    EXPECT_EQ(second.events.front().sequence, 2U);
    EXPECT_EQ(second.events.back().sequence, 2U);
    ASSERT_TRUE(second.latest_quote);
    EXPECT_EQ(second.latest_quote->bid_price.ToString(), "201.01");
    EXPECT_FALSE(second.gap_detected);
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

}  // namespace
