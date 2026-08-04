#include "tradebox/ui/chart_view.h"

namespace tradebox::ui {

ChartViewSeries CopyChartViewSeries(const std::vector<Bar>& source) {
    ChartViewSeries copy;
    copy.bars = source;
    return copy;
}

const char* ChartViewDataStateLabel(ChartViewDataState state) {
    switch (state) {
    case ChartViewDataState::Loading: return "Loading";
    case ChartViewDataState::Live: return "Live";
    case ChartViewDataState::Cached: return "Cached";
    case ChartViewDataState::Stale: return "Stale";
    case ChartViewDataState::NoData: return "No data";
    }
    return "No data";
}

}  // namespace tradebox::ui
