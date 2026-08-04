#pragma once

#include "tradebox/ui/model.h"

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

[[nodiscard]] ChartViewSeries CopyChartViewSeries(
    const std::vector<Bar>& source);
[[nodiscard]] const char* ChartViewDataStateLabel(ChartViewDataState state);

// The renderer receives an immutable, UI-owned copy. ImPlot remains confined
// to this adapter and is not part of the UI data model.
void RenderChartView(std::string_view plot_id,
                     std::string_view timeframe,
                     const ChartViewSeries& series,
                     int visible_bars,
                     ChartViewDataState state);

}  // namespace tradebox::ui
