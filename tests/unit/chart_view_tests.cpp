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
