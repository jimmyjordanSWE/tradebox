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

TEST(BarStore, UpdatedStreamBarWinsOverLateNormalDelivery) {
    BarStore store;
    MarketBar normal = Bar(60, "100");
    normal.source = BarSource::ProviderStream;
    MarketBar updated = Bar(60, "101");
    updated.source = BarSource::ProviderStream;
    updated.state = BarState::Revised;

    EXPECT_TRUE(store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {normal},
    }));
    EXPECT_TRUE(store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {updated},
    }));
    EXPECT_FALSE(store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {normal},
    }));
    EXPECT_FALSE(store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {updated},
    }));

    const auto result = store.Bars(Key(), {0, 120});
    ASSERT_EQ(result.bars.size(), 1U);
    EXPECT_EQ(result.bars[0].close.ToString(), "101");
    EXPECT_EQ(result.bars[0].state, BarState::Revised);
    EXPECT_EQ(result.bars[0].revision, 2U);
}

TEST(BarStore, UpdatedStreamBarIsRevisedWhenItArrivesFirst) {
    BarStore store;
    MarketBar updated = Bar(60, "101");
    updated.source = BarSource::ProviderStream;
    updated.state = BarState::Revised;
    store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {updated},
    });

    const auto result = store.Bars(Key(), {0, 120});
    ASSERT_EQ(result.bars.size(), 1U);
    EXPECT_EQ(result.bars[0].state, BarState::Revised);
}

TEST(BarStore, ProviderOpenBarReopensCurrentInterval) {
    BarStore store;
    MarketBar historical = Bar(60, "100");
    MarketBar live = Bar(60, "101");
    live.source = BarSource::ProviderStream;
    live.state = BarState::Open;
    store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {historical},
    });
    store.Upsert({
        .key = Key(),
        .symbol = "QQQ",
        .bars = {live},
    });

    const auto result = store.Bars(Key(), {0, 120});
    ASSERT_EQ(result.bars.size(), 1U);
    EXPECT_EQ(result.bars[0].close.ToString(), "101");
    EXPECT_EQ(result.bars[0].state, BarState::Open);
}

TEST(BarSeriesConvergence,
     ProviderFinalizationSupersedesMatchingProvisional) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    BarSeriesSnapshot snapshot{
        .key = Key(),
        .symbol = "QQQ",
        .requested_range = {0, 3 * minute},
        .bars = {Bar(minute)},
    };
    MarketDataSnapshot live{
        .instrument_id = Key().instrument_id,
        .symbol = "QQQ",
        .feed = MarketDataFeed::Iex,
        .stream_status = MarketStreamStatus::Subscribed,
        .trades_subscribed = true,
        .latest_price = CanonicalMarketPrice{.price = D("101")},
        .provisional_minute_bars = {{
            .start_ns = minute,
            .open = D("100"),
            .high = D("101"),
            .low = D("99"),
            .close = D("101"),
            .volume = D("10"),
            .trade_count = 2,
        }},
    };

    ConvergeLiveBar(snapshot, live);
    EXPECT_FALSE(snapshot.current_bar);
    ASSERT_TRUE(snapshot.latest_price);
    EXPECT_EQ(snapshot.latest_price->price.ToString(), "101");
}

TEST(BarSeriesConvergence,
     KeepsOnlyNewestOpenMinuteWithoutSynthesizingGaps) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    BarSeriesSnapshot snapshot{
        .key = Key(),
        .symbol = "QQQ",
        .requested_range = {0, 5 * minute},
        .bars = {Bar(minute)},
    };
    MarketDataSnapshot live{
        .instrument_id = Key().instrument_id,
        .symbol = "QQQ",
        .feed = MarketDataFeed::Iex,
        .stream_status = MarketStreamStatus::Subscribed,
        .trades_subscribed = true,
        .provisional_minute_bars = {
            {.start_ns = minute, .revision = 1},
            {.start_ns = 3 * minute, .revision = 2},
        },
    };

    ConvergeLiveBar(snapshot, live);
    ASSERT_EQ(snapshot.bars.size(), 1U);
    ASSERT_TRUE(snapshot.current_bar);
    EXPECT_EQ(snapshot.current_bar->start_ns, 3 * minute);
}

TEST(BarSeriesConvergence,
     NeverMixesFeedOrAdjustedHistoryWithRawLiveMinute) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    MarketDataSnapshot live{
        .instrument_id = Key().instrument_id,
        .symbol = "QQQ",
        .feed = MarketDataFeed::Sip,
        .stream_status = MarketStreamStatus::Subscribed,
        .trades_subscribed = true,
        .provisional_minute_bars = {{.start_ns = minute}},
    };
    BarSeriesSnapshot wrong_feed{
        .key = Key(MarketDataFeed::Iex),
        .requested_range = {0, 2 * minute},
    };
    ConvergeLiveBar(wrong_feed, live);
    EXPECT_FALSE(wrong_feed.current_bar);

    MarketDataSnapshot unsubscribed = live;
    unsubscribed.trades_subscribed = false;
    BarSeriesSnapshot inactive_symbol{
        .key = Key(MarketDataFeed::Sip),
        .requested_range = {0, 2 * minute},
    };
    ConvergeLiveBar(inactive_symbol, unsubscribed);
    EXPECT_FALSE(inactive_symbol.current_bar);

    BarSeriesKey adjusted_key = Key(MarketDataFeed::Sip);
    adjusted_key.adjustment = BarAdjustment::All;
    BarSeriesSnapshot adjusted{
        .key = adjusted_key,
        .requested_range = {0, 2 * minute},
    };
    ConvergeLiveBar(adjusted, live);
    EXPECT_FALSE(adjusted.current_bar);
}

TEST(BarSeriesConvergence,
     AggregatesGenericCurrentFiveMinuteCandle) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    BarSeriesKey key = Key();
    key.timeframe = "5Min";
    BarSeriesSnapshot snapshot{
        .key = key,
        .symbol = "QQQ",
        .requested_range = {10 * minute, 15 * minute},
        .bars = {Bar(10 * minute, "98")},
    };
    MarketDataSnapshot live{
        .instrument_id = key.instrument_id,
        .symbol = "QQQ",
        .feed = MarketDataFeed::Iex,
        .stream_status = MarketStreamStatus::Subscribed,
        .trades_subscribed = true,
        .projection_started_at_ns = 10 * minute,
        .provisional_minute_bars = {{
            .start_ns = 13 * minute,
            .open = D("104"),
            .high = D("106"),
            .low = D("103"),
            .close = D("105"),
            .volume = D("10"),
            .trade_count = 2,
            .revision = 7,
        }},
    };

    ConvergeLiveBar(
        snapshot, live,
        {Bar(10 * minute, "100"),
         Bar(11 * minute, "102"),
         Bar(12 * minute, "101")});

    EXPECT_TRUE(snapshot.bars.empty());
    ASSERT_TRUE(snapshot.current_bar);
    EXPECT_EQ(snapshot.current_bar->start_ns, 10 * minute);
    EXPECT_EQ(snapshot.current_bar->open.ToString(), "99.5");
    EXPECT_EQ(snapshot.current_bar->high.ToString(), "106");
    EXPECT_EQ(snapshot.current_bar->low.ToString(), "99");
    EXPECT_EQ(snapshot.current_bar->close.ToString(), "105");
    EXPECT_EQ(snapshot.current_bar->volume.ToString(), "37045");
    EXPECT_EQ(snapshot.current_bar->trade_count, 752U);
    EXPECT_EQ(snapshot.current_bar->state, BarState::Open);
    EXPECT_EQ(snapshot.current_bar->source, BarSource::DerivedTicks);
}

TEST(BarSeriesConvergence,
     DoesNotInventLargerCandleAfterMidIntervalReconnect) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    BarSeriesKey key = Key();
    key.timeframe = "5Min";
    BarSeriesSnapshot snapshot{
        .key = key,
        .requested_range = {10 * minute, 15 * minute},
        .bars = {Bar(10 * minute)},
    };
    MarketDataSnapshot live{
        .instrument_id = key.instrument_id,
        .symbol = "QQQ",
        .feed = MarketDataFeed::Iex,
        .stream_status = MarketStreamStatus::Subscribed,
        .trades_subscribed = true,
        .projection_started_at_ns = 11 * minute,
        .provisional_minute_bars = {{
            .start_ns = 12 * minute,
        }},
    };

    ConvergeLiveBar(snapshot, live, {Bar(11 * minute)});
    EXPECT_FALSE(snapshot.current_bar);
    ASSERT_EQ(snapshot.bars.size(), 1U);
    EXPECT_EQ(snapshot.bars[0].start_ns, 10 * minute);
}

TEST(BarSeriesConvergence,
     ProviderDailyOpenBarUsesGenericCurrentSlot) {
    BarSeriesKey key = Key();
    key.timeframe = "1Day";
    MarketBar daily = Bar(0);
    daily.state = BarState::Open;
    BarSeriesSnapshot snapshot{
        .key = key,
        .requested_range = {0, 1},
        .bars = {daily},
    };

    ConvergeLiveBar(snapshot, {});
    EXPECT_TRUE(snapshot.bars.empty());
    ASSERT_TRUE(snapshot.current_bar);
    EXPECT_EQ(snapshot.current_bar->state, BarState::Open);
}

TEST(BarSeriesConvergence, ParsesEveryFixedIntradayForm) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    EXPECT_EQ(FixedBarDurationNs("1Min"), minute);
    EXPECT_EQ(FixedBarDurationNs("59T"), 59 * minute);
    EXPECT_EQ(FixedBarDurationNs("1Hour"), 60 * minute);
    EXPECT_EQ(FixedBarDurationNs("23H"), 23 * 60 * minute);
    EXPECT_FALSE(FixedBarDurationNs("1Day"));
    EXPECT_FALSE(FixedBarDurationNs("60Min"));
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
