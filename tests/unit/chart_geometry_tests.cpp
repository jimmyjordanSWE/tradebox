#include "chart_geometry.h"

#include <gtest/gtest.h>

#include <limits>

namespace tradebox::gui::chart {
namespace {

core::Decimal D(std::string_view value) {
    const auto parsed = core::Decimal::Parse(value);
    EXPECT_TRUE(parsed);
    return parsed ? *parsed : core::Decimal::Zero();
}

core::MarketBar Bar(std::int64_t time, std::string_view open,
                    std::string_view high, std::string_view low,
                    std::string_view close,
                    std::string_view volume = "0") {
    return {
        .start_ns = time,
        .open = D(open),
        .high = D(high),
        .low = D(low),
        .close = D(close),
        .volume = D(volume),
    };
}

TEST(ChartGeometry, SelectsVisibleBarsWithoutSorting) {
    const std::vector<core::MarketBar> bars{
        Bar(10, "1", "2", "0", "1"),
        Bar(20, "2", "3", "1", "2"),
        Bar(30, "3", "4", "2", "3"),
    };
    const auto visible = SelectVisibleIndices(bars, {15, 31});
    EXPECT_EQ(visible.first, 1U);
    EXPECT_EQ(visible.last, 3U);
}

TEST(ChartGeometry, AutoscaleHandlesFlatNegativeAndEmptyData) {
    const std::vector<core::MarketBar> flat{
        Bar(10, "-5", "-5", "-5", "-5")};
    const auto scale = AutoscalePrices(flat, {0, 1});
    ASSERT_TRUE(scale);
    EXPECT_LT(scale->minimum, -5.0);
    EXPECT_GT(scale->maximum, -5.0);
    EXPECT_FALSE(AutoscalePrices(flat, {0, 0}));
}

TEST(ChartGeometry, PriceAndTimeTransformsAreInvertible) {
    const Rect plot{10.0f, 20.0f, 210.0f, 220.0f};
    const PriceScale scale{0.0, 100.0};
    EXPECT_FLOAT_EQ(scale.ToY(25.0, plot), 170.0f);
    EXPECT_DOUBLE_EQ(scale.FromY(170.0f, plot), 25.0);
    const core::BarRange range{100, 1'100};
    const float x = TimeToX(600, range, plot);
    EXPECT_FLOAT_EQ(x, 110.0f);
    EXPECT_EQ(XToTime(x, range, plot), 600);
}

TEST(ChartGeometry, BuildsCandleAndVolumeGeometrySafely) {
    const auto bar = Bar(500, "10", "14", "8", "12", "75");
    const Rect plot{0.0f, 0.0f, 100.0f, 100.0f};
    const auto candle = MakeCandleGeometry(
        bar, {0.0, 20.0}, {0, 1'000}, plot, 3.0f);
    EXPECT_TRUE(candle.rising);
    EXPECT_FALSE(candle.unchanged);
    EXPECT_LT(candle.high_y, candle.low_y);
    EXPECT_FLOAT_EQ(VolumeToY(75.0, 100.0, plot), 25.0f);
    const auto hit = HitTestBar(
        std::span<const core::MarketBar>(&bar, 1), {0, 1}, 490, {0, 1'000});
    ASSERT_TRUE(hit);
    EXPECT_EQ(*hit, 0U);
}

TEST(ChartGeometry, ZoomKeepsPointerTimeStable) {
    const Rect plot{0.0F, 0.0F, 1'000.0F, 500.0F};
    const core::BarRange range{1'000, 11'000};
    const auto zoomed = ZoomViewport(
        {.visible_bars = 100, .anchor_ns = 10'000}, range,
        250.0F, plot, 1.0F);
    ASSERT_TRUE(zoomed);
    EXPECT_LT(zoomed->visible_bars, 100);

    const std::int64_t pointed = XToTime(250.0F, range, plot);
    const long double ratio =
        static_cast<long double>(zoomed->visible_bars) / 100.0L;
    const auto expected = static_cast<std::int64_t>(
        static_cast<long double>(pointed) +
        (10'000.0L - static_cast<long double>(pointed)) * ratio);
    EXPECT_NEAR(static_cast<double>(zoomed->anchor_ns),
                static_cast<double>(expected), 1.0);
}

TEST(ChartGeometry, ZoomAndPanRejectInvalidGeometryAndClampLimits) {
    const Rect plot{0.0F, 0.0F, 1'000.0F, 500.0F};
    const core::BarRange range{1'000, 11'000};
    const auto maximum = ZoomViewport(
        {.visible_bars = 1'999, .anchor_ns = 10'000}, range,
        500.0F, plot, -100.0F);
    ASSERT_TRUE(maximum);
    EXPECT_EQ(maximum->visible_bars, 2'000);
    EXPECT_FALSE(ZoomViewport(
        {.visible_bars = 100, .anchor_ns = 10'000}, range,
        500.0F, {}, 1.0F));

    const auto panned = PanViewportAnchor(
        10'000, range, 100.0F, plot);
    ASSERT_TRUE(panned);
    EXPECT_EQ(*panned, 9'000);
    EXPECT_FALSE(PanViewportAnchor(10'000, range, 100.0F, {}));
}

TEST(ChartGeometry, ShiftRangeSaturatesAtTimestampBounds) {
    EXPECT_EQ(ShiftRange({10, 20}, -15), (core::BarRange{0, 5}));
    const auto shifted = ShiftRange(
        {std::numeric_limits<std::int64_t>::max() - 5,
         std::numeric_limits<std::int64_t>::max()},
        10);
    EXPECT_EQ(shifted.start_ns, std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(shifted.end_ns, std::numeric_limits<std::int64_t>::max());
}

TEST(ChartGeometry, ScreenAggregationPreservesOhlcvMeaning) {
    const std::vector<core::MarketBar> bars{
        Bar(1, "10", "12", "9", "11", "100"),
        Bar(2, "11", "15", "8", "14", "200"),
        Bar(50, "14", "16", "13", "15", "300")};
    const auto aggregated = AggregateBarsByScreenColumn(
        bars, {.first = 0, .last = bars.size()}, {0, 100},
        {0.0F, 0.0F, 2.0F, 100.0F});
    ASSERT_EQ(aggregated.size(), 2U);
    EXPECT_EQ(aggregated.front().open, D("10"));
    EXPECT_EQ(aggregated.front().high, D("15"));
    EXPECT_EQ(aggregated.front().low, D("8"));
    EXPECT_EQ(aggregated.front().close, D("14"));
    EXPECT_EQ(aggregated.front().volume, D("300"));
}

}  // namespace
}  // namespace tradebox::gui::chart
