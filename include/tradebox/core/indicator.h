#pragma once

#include "tradebox/core/bar_series.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace tradebox::core {

enum class BarSeriesField {
    Open,
    High,
    Low,
    Close,
    Volume,
};

struct BarSeriesInput {
    BarSeriesField field = BarSeriesField::Close;

    bool operator==(const BarSeriesInput&) const = default;
};

struct IndicatorOutputInput {
    std::string indicator_id;
    std::string output_id = "value";

    bool operator==(const IndicatorOutputInput&) const = default;
};

using IndicatorInput =
    std::variant<BarSeriesInput, IndicatorOutputInput>;

struct SimpleMovingAverageCalculation {
    IndicatorInput input = BarSeriesInput{};
    std::uint32_t period = 20;

    bool operator==(const SimpleMovingAverageCalculation&) const = default;
};

struct ExponentialMovingAverageCalculation {
    IndicatorInput input = BarSeriesInput{};
    std::uint32_t period = 20;

    bool operator==(const ExponentialMovingAverageCalculation&) const = default;
};

using IndicatorCalculation = std::variant<
    SimpleMovingAverageCalculation,
    ExponentialMovingAverageCalculation>;

// This graph node is renderer- and syntax-independent. A future formula
// parser and a visual Lego editor both compile to this representation.
struct IndicatorDefinition {
    std::string id;
    IndicatorCalculation calculation =
        SimpleMovingAverageCalculation{};

    bool operator==(const IndicatorDefinition&) const = default;
};

struct IndicatorPoint {
    std::int64_t start_ns = 0;
    Decimal value;

    bool operator==(const IndicatorPoint&) const = default;
};

struct IndicatorSeries {
    std::string indicator_id;
    std::string output_id = "value";
    std::vector<IndicatorPoint> points;
};

struct IndicatorGraphError {
    std::string indicator_id;
    std::string message;
};

[[nodiscard]] std::expected<std::vector<IndicatorSeries>, IndicatorGraphError>
EvaluateIndicators(std::span<const IndicatorDefinition> definitions,
                   std::span<const MarketBar> bars);

}  // namespace tradebox::core
