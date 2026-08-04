#include "tradebox/ui/chart_view.h"

#include <gtest/gtest.h>

TEST(ChartViewData, CopiesSeriesBeforeRendering) {
    const std::vector<Bar> source = {
        {1'700'000'000'000, 100, 102, 99, 101, 500},
        {1'700'000'060'000, 101, 103, 100, 102, 700},
    };

    auto copy = tradebox::ui::CopyChartViewSeries(source);

    ASSERT_EQ(copy.bars.size(), source.size());
    EXPECT_EQ(copy.bars[0].timestamp_ms, source[0].timestamp_ms);
    EXPECT_EQ(copy.bars[1].close, source[1].close);
}

TEST(ChartViewData, CopyDoesNotAliasSource) {
    std::vector<Bar> source = {{1'700'000'000'000, 100, 102, 99, 101, 500}};
    auto copy = tradebox::ui::CopyChartViewSeries(source);

    source[0].close = 999;
    source.clear();

    ASSERT_EQ(copy.bars.size(), 1U);
    EXPECT_DOUBLE_EQ(copy.bars[0].close, 101);
}

TEST(ChartViewData, CopiesCanonicalBarSnapshotAndCurrentBar) {
    tradebox::core::BarSeriesSnapshot source;
    source.bars.push_back({
        .start_ns = 1'700'000'000'000LL * 1'000'000,
        .open = *tradebox::core::Decimal::Parse("100"),
        .high = *tradebox::core::Decimal::Parse("102"),
        .low = *tradebox::core::Decimal::Parse("99"),
        .close = *tradebox::core::Decimal::Parse("101"),
        .volume = *tradebox::core::Decimal::Parse("500"),
    });
    source.current_bar = tradebox::core::MarketBar{
        .start_ns = 1'700'000'060'000LL * 1'000'000,
        .open = *tradebox::core::Decimal::Parse("101"),
        .high = *tradebox::core::Decimal::Parse("103"),
        .low = *tradebox::core::Decimal::Parse("100"),
        .close = *tradebox::core::Decimal::Parse("102"),
        .volume = *tradebox::core::Decimal::Parse("700"),
    };

    const auto copy = tradebox::ui::CopyChartViewSeries(source);

    ASSERT_EQ(copy.bars.size(), 2U);
    EXPECT_DOUBLE_EQ(copy.bars.back().close, 102);
}
