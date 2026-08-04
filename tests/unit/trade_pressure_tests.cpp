#include "tradebox/core/trade_pressure.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

namespace {

using namespace tradebox::core;

Decimal D(const char* value) {
    return *Decimal::Parse(value);
}

MarketQuote Quote(const char* bid, const char* ask,
                  std::int64_t time_ms) {
    return {
        .bid_price = D(bid),
        .ask_price = D(ask),
        .event_time_ns = time_ms * 1'000'000,
    };
}

MarketTrade Trade(const char* id, const char* price,
                  const char* size = "1",
                  std::int64_t time_ms = 1'000) {
    return {
        .trade_id = id,
        .price = D(price),
        .size = D(size),
        .event_time_ns = time_ms * 1'000'000,
    };
}

TEST(TradePressure, UsesQuoteTestAtAndBeyondTheSpread) {
    TradePressureReducer reducer({
        .half_life_ms = 500,
        .quote_max_age_ms = 2'000,
        .tick_direction_max_age_ms = 5'000,
        .correction_horizon_ms = 30'000,
        .maximum_recent_contributions = 512,
        .weight = TradePressureWeight::Trades,
    });
    reducer.ObserveQuote(Quote("99", "101", 1'000), 1'000);

    reducer.ObserveTrade(Trade("buyer", "101", "1", 1'001), 1'001);
    auto snapshot = reducer.Snapshot(1'001);
    EXPECT_EQ(snapshot.signed_pressure, 1.0);
    EXPECT_EQ(snapshot.latest_method, TradeClassificationMethod::QuoteTest);

    reducer.ObserveTrade(Trade("seller", "99", "1", 1'002), 1'002);
    snapshot = reducer.Snapshot(1'002);
    EXPECT_LT(snapshot.signed_pressure, 0.0);
    EXPECT_EQ(snapshot.latest_method, TradeClassificationMethod::QuoteTest);
}

TEST(TradePressure, FallsBackToTickForInvalidOrInsideSpreadQuotes) {
    TradePressureReducer reducer({
        .half_life_ms = 500,
        .quote_max_age_ms = 2'000,
        .tick_direction_max_age_ms = 5'000,
        .correction_horizon_ms = 30'000,
        .maximum_recent_contributions = 512,
        .weight = TradePressureWeight::Trades,
    });
    reducer.ObserveQuote(Quote("99", "101", 1'000), 1'000);
    reducer.ObserveTrade(Trade("first", "100", "1", 1'001), 1'001);
    reducer.ObserveTrade(Trade("up", "100.5", "1", 1'002), 1'002);
    EXPECT_EQ(reducer.Snapshot(1'002).latest_method,
              TradeClassificationMethod::TickTest);

    reducer.ObserveQuote(Quote("101", "101", 1'003), 1'003);
    reducer.ObserveTrade(Trade("down", "99", "1", 1'003), 1'003);
    reducer.ObserveTrade(Trade("equal", "99", "1", 1'004), 1'004);
    EXPECT_EQ(reducer.Snapshot(1'004).latest_method,
              TradeClassificationMethod::TickTest);
    EXPECT_EQ(reducer.Snapshot(1'004).seller_trades, 2U);
}

TEST(TradePressure, RejectsStaleAndFutureQuotes) {
    TradePressureReducer reducer({
        .quote_max_age_ms = 10,
        .weight = TradePressureWeight::Trades,
    });
    reducer.ObserveQuote(Quote("99", "101", 1'000), 1'000);
    reducer.ObserveTrade(Trade("first", "100", "1", 1'020), 1'020);
    reducer.ObserveTrade(Trade("second", "101", "1", 1'021), 1'021);
    EXPECT_EQ(reducer.Snapshot(1'021).latest_method,
              TradeClassificationMethod::TickTest);

    reducer.ObserveQuote(Quote("102", "103", 1'030), 1'030);
    reducer.ObserveTrade(Trade("future", "103", "1", 1'029), 1'029);
    EXPECT_EQ(reducer.Snapshot(1'030).latest_method,
              TradeClassificationMethod::TickTest);
}

TEST(TradePressure, EqualPriceTickDirectionExpires) {
    TradePressureReducer reducer({
        .tick_direction_max_age_ms = 10,
        .weight = TradePressureWeight::Trades,
    });
    reducer.ObserveTrade(Trade("first", "100", "1", 1'000), 1'000);
    reducer.ObserveTrade(Trade("up", "101", "1", 1'001), 1'001);
    reducer.ObserveTrade(Trade("equal-fresh", "101", "1", 1'010), 1'010);
    EXPECT_EQ(reducer.Snapshot(1'010).buyer_trades, 2U);

    reducer.ObserveTrade(Trade("equal-expired", "101", "1", 1'012), 1'012);
    const auto snapshot = reducer.Snapshot(1'012);
    EXPECT_EQ(snapshot.unknown_trades, 2U);
    EXPECT_EQ(snapshot.latest_method,
              TradeClassificationMethod::Unknown);
}

TEST(TradePressure, DecaysActivityAndSaturatesBurst) {
    TradePressureReducer reducer({
        .half_life_ms = 100,
        .weight = TradePressureWeight::Trades,
        .activity_saturation_weight = 1.0,
    });
    reducer.ObserveTrade(Trade("one", "100", "1", 1'000), 1'000);
    const double initial = reducer.Snapshot(1'000).activity;
    reducer.ObserveTrade(Trade("two", "101", "1", 1'001), 1'001);
    const double burst = reducer.Snapshot(1'001).activity;
    const double faded = reducer.Snapshot(2'001).activity;
    EXPECT_GT(burst, initial);
    EXPECT_LT(faded, burst);
    EXPECT_GE(faded, 0.0);
    EXPECT_LE(faded, 1.0);
    EXPECT_GE(reducer.Snapshot(1'001).signed_pressure, -1.0);
    EXPECT_LE(reducer.Snapshot(1'001).signed_pressure, 1.0);
}

TEST(TradePressure, UnknownObservationsDoNotInventDirection) {
    TradePressureReducer reducer({
        .half_life_ms = 500,
        .quote_max_age_ms = 2'000,
        .tick_direction_max_age_ms = 5'000,
        .correction_horizon_ms = 30'000,
        .maximum_recent_contributions = 512,
        .weight = TradePressureWeight::Trades,
    });
    reducer.ObserveTrade(Trade("unknown", "100", "1", 1'000), 1'000);
    const auto snapshot = reducer.Snapshot(1'000);
    EXPECT_EQ(snapshot.signed_pressure, 0.0);
    EXPECT_EQ(snapshot.activity, 0.0);
    EXPECT_EQ(snapshot.unknown_weight, 1.0);
    EXPECT_EQ(snapshot.unknown_trades, 1U);
}

TEST(TradePressure, OutOfOrderObservationCannotReverseDecayTime) {
    TradePressureReducer reducer({
        .half_life_ms = 100,
        .weight = TradePressureWeight::Trades,
    });
    reducer.ObserveTrade(Trade("first", "100", "1", 1'000), 1'000);
    reducer.ObserveTrade(Trade("up", "101", "1", 1'200), 1'200);
    const auto before = reducer.Snapshot(1'200);
    reducer.ObserveTrade(Trade("late", "102", "1", 1'100), 1'100);
    const auto after = reducer.Snapshot(1'100);
    EXPECT_EQ(after.last_observation_ms, 1'200);
    EXPECT_GE(after.activity, before.activity);
}

TEST(TradePressure, CorrectionsReplaceAndCancellationsRemoveContributions) {
    TradePressureReducer reducer({
        .weight = TradePressureWeight::Trades,
    });
    reducer.ObserveQuote(Quote("99", "101", 1'000), 1'000);
    reducer.ObserveTrade(Trade("trade", "101", "1", 1'001), 1'001);
    reducer.CorrectTrade("trade", Trade("trade-corrected", "99", "1", 1'002),
                         1'002);
    auto snapshot = reducer.Snapshot(1'002);
    EXPECT_EQ(snapshot.seller_trades, 1U);
    EXPECT_EQ(snapshot.buyer_trades, 0U);

    EXPECT_TRUE(reducer.CancelTrade("trade-corrected", 1'002, 1'003));
    snapshot = reducer.Snapshot(1'003);
    EXPECT_EQ(snapshot.buyer_trades, 0U);
    EXPECT_EQ(snapshot.seller_trades, 0U);
    EXPECT_EQ(snapshot.activity, 0.0);
}

TEST(TradePressure, DoesNotReconstructContributionsOutsideCorrectionHorizon) {
    TradePressureReducer reducer({
        .correction_horizon_ms = 10,
        .weight = TradePressureWeight::Trades,
    });
    reducer.ObserveTrade(Trade("old", "100", "1", 1'000), 1'000);
    EXPECT_FALSE(reducer.CancelTrade("old", 1'000, 1'011));
    EXPECT_FALSE(reducer.CorrectTrade(
        "old", Trade("replacement", "101", "1", 1'012), 1'012));
    EXPECT_EQ(reducer.RecentContributionCount(), 0U);
    EXPECT_EQ(reducer.Snapshot(1'012).activity, 0.0);
}

TEST(TradePressure, ContributionMemoryStaysBounded) {
    TradePressureReducer reducer({
        .maximum_recent_contributions = 3,
        .weight = TradePressureWeight::Trades,
    });
    for (int index = 0; index < 20; ++index)
        reducer.ObserveTrade(
            Trade(std::to_string(index).c_str(), "100", "1", 1'000 + index),
            1'000 + index);
    EXPECT_EQ(reducer.RecentContributionCount(), 3U);
}

TEST(TradePressure, MarkStaleClearsTransientClassificationState) {
    TradePressureReducer reducer;
    reducer.ObserveQuote(Quote("99", "101", 1'000), 1'000);
    reducer.ObserveTrade(Trade("trade", "101", "10", 1'001), 1'001);
    reducer.MarkStale();
    const auto snapshot = reducer.Snapshot(1'002);
    EXPECT_TRUE(snapshot.stale);
    EXPECT_EQ(snapshot.activity, 0.0);
    EXPECT_EQ(snapshot.last_observation_ms, 0);
    EXPECT_EQ(reducer.RecentContributionCount(), 0U);
}

}  // namespace
