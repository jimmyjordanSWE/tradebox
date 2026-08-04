#pragma once

#include "tradebox/ui/model.h"
#include "tradebox/core/bar_series.h"

#include <string_view>
#include <vector>

namespace tradebox::ui {

enum class ChartViewDataState {
    Loading,
    Live,
    Cached,
    Stale,
    NoData,
};

struct ChartViewSeries {
    std::vector<Bar> bars;
};

// Rendering policy only. The core owns the market-data semantics; these
// options describe which already-projected fields the adapter should draw.
struct ChartViewOptions {
    bool show_volume = true;
    bool show_close_line = false;
    bool show_crosshair = true;
};

[[nodiscard]] ChartViewSeries CopyChartViewSeries(
    const std::vector<Bar>& source);
[[nodiscard]] ChartViewSeries CopyChartViewSeries(
    const tradebox::core::BarSeriesSnapshot& source);
[[nodiscard]] const char* ChartViewDataStateLabel(ChartViewDataState state);

// The renderer receives an immutable, UI-owned copy. ImPlot remains confined
// to this adapter and is not part of the UI data model.
void RenderChartView(std::string_view plot_id,
                     std::string_view timeframe,
                     const ChartViewSeries& series,
                     int visible_bars,
                     ChartViewDataState state,
                     const ChartViewOptions& options = {});

}  // namespace tradebox::ui
