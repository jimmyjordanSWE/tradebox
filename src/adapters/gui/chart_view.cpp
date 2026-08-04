#include "tradebox/ui/chart_view.h"

#include "implot.h"

#include <algorithm>
#include <string>
#include <vector>

namespace tradebox::ui {

void RenderChartView(std::string_view plot_id,
                     std::string_view timeframe,
                     const ChartViewSeries& series,
                     int visible_bars,
                     ChartViewDataState state) {
    ImGui::TextDisabled("%s", ChartViewDataStateLabel(state));
    if (series.bars.empty()) {
        if (state == ChartViewDataState::Loading) {
            ImGui::TextDisabled("Waiting for %.*s bars...",
                                static_cast<int>(timeframe.size()),
                                timeframe.data());
        } else {
            ImGui::TextDisabled("No %.*s bars available.",
                                static_cast<int>(timeframe.size()),
                                timeframe.data());
        }
        return;
    }

    const int count = std::min(
        std::max(visible_bars, 1), static_cast<int>(series.bars.size()));
    const int first = static_cast<int>(series.bars.size()) - count;
    std::vector<double> timestamps;
    std::vector<double> closes;
    timestamps.reserve(static_cast<std::size_t>(count));
    closes.reserve(static_cast<std::size_t>(count));
    for (int index = first; index < static_cast<int>(series.bars.size());
         ++index) {
        timestamps.push_back(
            static_cast<double>(series.bars[index].timestamp_ms) / 1000.0);
        closes.push_back(series.bars[index].close);
    }

    const std::string title = std::string(plot_id);
    if (!ImPlot::BeginPlot(title.c_str(), ImVec2(-1, 250))) return;
    ImPlot::SetupAxes("Time", "Price");
    ImPlot::SetupAxisFormat(ImAxis_X1, "%H:%M");
    ImPlot::SetupAxisFormat(ImAxis_Y1, "%.2f");
    ImPlot::PlotLine("Close", timestamps.data(), closes.data(), count);
    ImPlot::EndPlot();
}

}  // namespace tradebox::ui
