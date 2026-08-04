#pragma once

#include "tradebox/core/bar_series.h"
#include "tradebox/core/market_data.h"
#include "tradebox/core/types.h"

#include <string>
#include <vector>

namespace tradebox::application {

struct UiChartQuery {
    std::string symbol;
    std::string timeframe;
    core::BarRange range;
};

struct UiSnapshotQuery {
    std::vector<std::string> market_symbols;
    std::vector<UiChartQuery> charts;
};

// A complete read model for one render pass. It contains no ImGui, SDL, or
// ImPlot types and can therefore be assembled and tested independently of the
// renderer.
struct ApplicationUiSnapshot {
    core::CoreSnapshot core;
    std::vector<core::MarketDataSnapshot> markets;
    std::vector<core::BarSeriesSnapshot> charts;
};

}  // namespace tradebox::application
