#pragma once

#include "tradebox/core/bar_series.h"
#include "tradebox/core/asset_catalog.h"
#include "tradebox/core/indicator.h"
#include "tradebox/core/market_data.h"
#include "tradebox/core/types.h"
#include "tradebox/application/indicator_projection_cache.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tradebox::application {

struct SavedAccountDescriptor {
    std::string credential_slot;
    core::AccountEnvironment environment = core::AccountEnvironment::Paper;
    std::string api_key_id;
};

struct UiChartQuery {
    std::string document_id;
    core::BarSeriesKey key;
    std::string symbol;
    core::BarRange range;
    std::vector<core::IndicatorDefinition> indicators;
};

struct UiWatchListRowQuery {
    std::string row_id;
    std::string instrument_id;
    std::string symbol;
};

struct UiWatchListQuery {
    std::string document_id;
    std::vector<UiWatchListRowQuery> rows;
    bool needs_change_from_open = false;
};

struct UiSnapshotQuery {
    std::int64_t as_of_ns = 0;
    std::vector<std::string> market_symbols;
    std::vector<UiChartQuery> charts;
    std::vector<UiWatchListQuery> watch_lists;
    std::string asset_search;
    std::vector<std::string> asset_searches;
    std::vector<std::string> asset_preferred_instrument_ids;
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

struct UiAssetSearchResult {
    std::string query;
    std::vector<core::TradableAsset> matches;
};

struct UiChartSnapshot {
    std::string document_id;
    ChartDataStatus status = ChartDataStatus::Unavailable;
    std::string message;
    bool retryable = false;
    core::BarSeriesSnapshot series;
    std::shared_ptr<const IndicatorProjectionSnapshot>
        indicator_projection;
};

struct UiWatchListRowSnapshot {
    std::string row_id;
    std::optional<core::Decimal> current_price;
    std::optional<core::Decimal> change_from_open;
    bool history_missing = false;
};

struct UiWatchListSnapshot {
    std::string document_id;
    core::BarRange daily_range;
    std::vector<UiWatchListRowSnapshot> rows;
};

// A complete read model for one render pass. It contains no renderer types and
// can therefore be assembled and tested independently of the renderer.
struct ApplicationUiSnapshot {
    core::CoreSnapshot core;
    core::MarketDataFrame markets;
    std::vector<UiChartSnapshot> charts;
    std::vector<UiWatchListSnapshot> watch_lists;
    std::vector<core::TradableAsset> assets;
    std::vector<UiAssetSearchResult> asset_search_results;
};

}  // namespace tradebox::application
