#include "tradebox/ui/chart_view.h"

namespace tradebox::ui {

ChartViewSeries CopyChartViewSeries(const std::vector<Bar>& source) {
    ChartViewSeries copy;
    copy.bars = source;
    return copy;
}

ChartViewSeries CopyChartViewSeries(
    const tradebox::core::BarSeriesSnapshot& source) {
    ChartViewSeries copy;
    copy.bars.reserve(source.bars.size() +
                      (source.current_bar ? 1U : 0U));
    const auto append = [&copy](const tradebox::core::MarketBar& bar) {
        copy.bars.push_back({
            .timestamp_ms = bar.start_ns / 1'000'000,
            .open = bar.open.ToDisplayDouble(),
            .high = bar.high.ToDisplayDouble(),
            .low = bar.low.ToDisplayDouble(),
            .close = bar.close.ToDisplayDouble(),
            .volume = bar.volume.ToDisplayDouble(),
        });
    };
    for (const auto& bar : source.bars) append(bar);
    if (source.current_bar) append(*source.current_bar);
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
