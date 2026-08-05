#pragma once

#include "tradebox/core/bar_series.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace tradebox::application {

struct ChartRangePolicy {
    std::size_t minimum_visible_bars = 30;
    std::size_t maximum_visible_bars = 2'000;
    std::size_t history_multiplier = 3;
    std::size_t future_bars = 1;
};

struct ChartViewportIntent {
    std::string document_id;
    core::BarSeriesKey key;
    std::string symbol;
    std::int64_t anchor_ns = 0;
    std::size_t visible_bars = 200;
};

[[nodiscard]] std::expected<core::BarRange, std::string>
ResolveChartRange(
    const ChartViewportIntent& intent,
    const ChartRangePolicy& policy = {});

}  // namespace tradebox::application
