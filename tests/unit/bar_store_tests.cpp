#include "tradebox/core/bar_store.h"

#include <gtest/gtest.h>

namespace {

using namespace tradebox::core;

Decimal D(std::string_view value) {
    return *Decimal::Parse(value);
}

BarSeriesKey Key(
    MarketDataFeed feed = MarketDataFeed::Iex) {
    return {
        .instrument_id = "isin:US46090E1038",
        .feed = feed,
        .timeframe = "1Min",
        .adjustment = BarAdjustment::Raw,
    };
}

MarketBar Bar(
    std::int64_t start, std::string_view close = "100") {
    return {
        .start_ns = start,
        .open = D("99.5"),
        .high = D("100.5"),
        .low = D("99"),
        .close = D(close),
        .volume = D("12345"),
        .within_bar_vwap = D("99.875"),
        .trade_count = 250,
        .source = BarSource::ProviderHistorical,
        .state = BarState::Finalized,
    };
}

TEST(BarStore, RetainsProviderBarsWithExactDecimals) {
    BarStore store;
    EXPECT_TRUE(store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {Bar(60), Bar(120, "100.125")},
        .covered_range = BarRange{60, 180},
    }));

    const auto result = store.Bars(Key(), {60, 180});
    ASSERT_EQ(result.bars.size(), 2U);
    EXPECT_EQ(result.bars[1].close.ToString(), "100.125");
    EXPECT_EQ(result.bars[1].volume.ToString(), "12345");
    EXPECT_EQ(result.bars[1].revision, 1U);
    EXPECT_TRUE(result.missing_ranges.empty());
}

TEST(BarStore, ReportsOnlyMissingCoverageAndMergesAdjacentRanges) {
    BarStore store;
    store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .covered_range = BarRange{100, 200},
    });
    store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .covered_range = BarRange{200, 300},
    });

    const auto result = store.Bars(Key(), {50, 350});
    EXPECT_EQ(
        result.missing_ranges,
        (std::vector<BarRange>{{50, 100}, {300, 350}}));
}

TEST(BarStore, RevisesAChangedFinalizedProviderBar) {
    BarStore store;
    store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {Bar(60)},
    });
    EXPECT_FALSE(store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {Bar(60)},
    }));
    EXPECT_TRUE(store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {Bar(60, "100.25")},
    }));

    const auto result = store.Bars(Key(), {0, 120});
    ASSERT_EQ(result.bars.size(), 1U);
    EXPECT_EQ(result.bars[0].close.ToString(), "100.25");
    EXPECT_EQ(result.bars[0].revision, 2U);
    EXPECT_EQ(result.bars[0].state, BarState::Revised);
}

TEST(BarStore, KeepsFeedsAsSeparateSeries) {
    BarStore store;
    store.Upsert({
        .key = Key(MarketDataFeed::Iex),
        .symbol = "QQQ",
        .bars = {Bar(60, "100")},
    });
    store.Upsert({
        .key = Key(MarketDataFeed::Sip),
        .symbol = "QQQ",
        .bars = {Bar(60, "101")},
    });

    EXPECT_EQ(
        store.Bars(Key(MarketDataFeed::Iex), {0, 120})
            .bars[0]
            .close.ToString(),
        "100");
    EXPECT_EQ(
        store.Bars(Key(MarketDataFeed::Sip), {0, 120})
            .bars[0]
            .close.ToString(),
        "101");
}

TEST(BarStore, CoalescesChangedSeriesAfterCursor) {
    BarStore store;
    store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {Bar(60)},
    });
    store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {Bar(120)},
    });

    const auto first = store.BarChanges(0, 10);
    ASSERT_EQ(first.series.size(), 1U);
    EXPECT_EQ(first.series[0].symbol, "QQQ");
    EXPECT_TRUE(
        store.BarChanges(first.next_sequence, 10).series.empty());
}

TEST(BarStore, ChangedSeriesRingReportsConsumerOverrun) {
    BarStore store(2);
    for (std::int64_t start = 1; start <= 4; ++start)
        store.Upsert({
            .key = Key(),
            .symbol = "QQQ",
            .bars = {Bar(start)},
        });

    const auto changes = store.BarChanges(1, 10);
    EXPECT_TRUE(changes.gap_detected);
    ASSERT_EQ(changes.series.size(), 1U);
    EXPECT_EQ(changes.series[0].sequence, 4U);
}

}  // namespace
