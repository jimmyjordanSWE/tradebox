#pragma once

#include "tradebox/core/bar_series.h"
#include "tradebox/core/asset_catalog.h"
#include "tradebox/core/indicator.h"
#include "tradebox/core/market_data.h"
#include "tradebox/core/types.h"

#include <string>
#include <vector>

namespace tradebox::application {

struct UiChartQuery {
    std::string document_id;
    core::BarSeriesKey key;
    std::string symbol;
    core::BarRange range;
    std::vector<core::IndicatorDefinition> indicators;
};

struct UiSnapshotQuery {
    std::vector<std::string> market_symbols;
    std::vector<UiChartQuery> charts;
    std::string asset_search;
    // Zero avoids catalog work during ordinary frames. The GUI opts in while
    // presenting ticker search results.
    std::size_t asset_limit = 0;
};

enum class ChartDataStatus {
    Unavailable,
    Loading,
    Ready,
    Empty,
    MissingHistory,
    Failed,
};

struct UiChartSnapshot {
    std::string document_id;
    ChartDataStatus status = ChartDataStatus::Unavailable;
    std::string message;
    bool retryable = false;
    core::BarSeriesSnapshot series;
    std::vector<core::IndicatorSeries> indicators;
    std::vector<std::string> indicator_errors;
};

// A complete read model for one render pass. It contains no renderer types and
// can therefore be assembled and tested independently of the renderer.
struct ApplicationUiSnapshot {
    core::CoreSnapshot core;
    std::vector<core::MarketDataSnapshot> markets;
    std::vector<UiChartSnapshot> charts;
    std::vector<core::TradableAsset> assets;
};

}  // namespace tradebox::application
