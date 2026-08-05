#include "tradebox/core/indicator.h"

#include <gtest/gtest.h>

namespace tradebox::core {
namespace {

Decimal D(std::string_view value) {
    const auto parsed = Decimal::Parse(value);
    if (!parsed) ADD_FAILURE() << parsed.error().message;
    return parsed ? *parsed : Decimal::Zero();
}

MarketBar Bar(std::int64_t start_ns, std::string_view close,
              std::string_view volume = "0") {
    const Decimal value = D(close);
    return {
        .start_ns = start_ns,
        .open = value,
        .high = value,
        .low = value,
        .close = value,
        .volume = D(volume),
    };
}

TEST(IndicatorGraph, CalculatesSimpleMovingAverageWithExactDecimals) {
    const std::vector<MarketBar> bars{
        Bar(1, "1"), Bar(2, "2"), Bar(3, "3"), Bar(4, "4")};
    const std::vector<IndicatorDefinition> definitions{{
        .id = "sma.3",
        .calculation = SimpleMovingAverageCalculation{.period = 3},
    }};
    const auto result = EvaluateIndicators(definitions, bars);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->front().points.size(), 2U);
    EXPECT_EQ(result->front().points[0].value, D("2"));
    EXPECT_EQ(result->front().points[1].value, D("3"));
}

TEST(IndicatorGraph, CalculatesExponentialMovingAverageFromSmaSeed) {
    const std::vector<MarketBar> bars{
        Bar(1, "1"), Bar(2, "2"), Bar(3, "3"), Bar(4, "4")};
    const std::vector<IndicatorDefinition> definitions{{
        .id = "ema.3",
        .calculation = ExponentialMovingAverageCalculation{.period = 3},
    }};
    const auto result = EvaluateIndicators(definitions, bars);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->front().points.size(), 2U);
    EXPECT_EQ(result->front().points[0].value, D("2"));
    EXPECT_EQ(result->front().points[1].value, D("3"));
}

TEST(IndicatorGraph, AcceptsVolumeAndAnotherIndicatorAsInputs) {
    const std::vector<MarketBar> bars{
        Bar(1, "10", "1"), Bar(2, "20", "3"),
        Bar(3, "30", "5"), Bar(4, "40", "7")};
    const std::vector<IndicatorDefinition> definitions{
        {
            .id = "volume.sma.2",
            .calculation = SimpleMovingAverageCalculation{
                .input = BarSeriesInput{BarSeriesField::Volume},
                .period = 2},
        },
        {
            .id = "smoothed.volume.sma.2",
            .calculation = SimpleMovingAverageCalculation{
                .input = IndicatorOutputInput{
                    "volume.sma.2", "value"},
                .period = 2},
        },
    };
    const auto result = EvaluateIndicators(definitions, bars);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ((*result)[1].points.size(), 2U);
    EXPECT_EQ((*result)[1].points[0].start_ns, 3);
    EXPECT_EQ((*result)[1].points[0].value, D("3"));
    EXPECT_EQ((*result)[1].points[1].value, D("5"));
}

TEST(IndicatorGraph, RejectsMissingReferencesCyclesAndInvalidPeriods) {
    std::vector<IndicatorDefinition> definitions{{
        .id = "broken",
        .calculation = SimpleMovingAverageCalculation{
            .input = IndicatorOutputInput{"missing", "value"}},
    }};
    EXPECT_FALSE(EvaluateIndicators(definitions, {}));
    definitions = {
        {.id = "a",
         .calculation = SimpleMovingAverageCalculation{
             .input = IndicatorOutputInput{"b", "value"}}},
        {.id = "b",
         .calculation = SimpleMovingAverageCalculation{
             .input = IndicatorOutputInput{"a", "value"}}},
    };
    EXPECT_FALSE(EvaluateIndicators(definitions, {}));
    definitions = {{
        .id = "invalid-period",
        .calculation = SimpleMovingAverageCalculation{.period = 0},
    }};
    EXPECT_FALSE(EvaluateIndicators(definitions, {}));
}

}  // namespace
}  // namespace tradebox::core
