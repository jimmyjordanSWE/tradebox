#include "chart_geometry.h"

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace tradebox::gui::chart
