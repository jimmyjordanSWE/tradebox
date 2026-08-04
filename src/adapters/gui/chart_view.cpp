#include "tradebox/ui/chart_view.h"

#include "implot.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace tradebox::ui {

namespace {

double CandleWidth(const std::vector<double>& timestamps) {
    double smallest_gap = 0.0;
    for (std::size_t index = 1; index < timestamps.size(); ++index) {
        const double gap = timestamps[index] - timestamps[index - 1];
        if (gap > 0.0 && (smallest_gap == 0.0 || gap < smallest_gap))
            smallest_gap = gap;
    }
    return smallest_gap > 0.0 ? smallest_gap * 0.72 : 60.0 * 0.72;
}

void PlotCandles(const std::vector<double>& timestamps,
                 const std::vector<double>& opens,
                 const std::vector<double>& highs,
                 const std::vector<double>& lows,
                 const std::vector<double>& closes) {
    const double width = CandleWidth(timestamps);
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    for (std::size_t index = 0; index < timestamps.size(); ++index) {
        const double x = timestamps[index];
        const bool bullish = closes[index] >= opens[index];
        const ImU32 color = ImGui::GetColorU32(
            bullish ? ImVec4(0.20f, 0.82f, 0.52f, 1.0f)
                    : ImVec4(0.93f, 0.31f, 0.38f, 1.0f));
        const ImVec2 wick_top = ImPlot::PlotToPixels(x, highs[index]);
        const ImVec2 wick_bottom = ImPlot::PlotToPixels(x, lows[index]);
        draw_list->AddLine(wick_top, wick_bottom, color, 1.0f);

        const ImVec2 body_left =
            ImPlot::PlotToPixels(x - width * 0.5, opens[index]);
        const ImVec2 body_right =
            ImPlot::PlotToPixels(x + width * 0.5, closes[index]);
        const ImVec2 minimum(
            std::min(body_left.x, body_right.x),
            std::min(body_left.y, body_right.y));
        const ImVec2 maximum(
            std::max(body_left.x, body_right.x),
            std::max(body_left.y, body_right.y));
        const float minimum_height = 1.0f;
        const ImVec2 adjusted_maximum(
            maximum.x, std::max(maximum.y, minimum.y + minimum_height));
        draw_list->AddRectFilled(minimum, adjusted_maximum, color);
        draw_list->AddRect(minimum, adjusted_maximum, color, 0.0f, 0, 1.0f);
    }
    ImPlot::PopPlotClipRect();
}

void DrawCrosshair(double x, double y) {
    bool hovered = false;
    ImPlot::DragLineX(0x4C554E41, &x, ImVec4(0.72f, 0.78f, 0.86f, 0.45f),
                      1.0f, ImPlotDragToolFlags_NoInputs, nullptr, &hovered);
    ImPlot::DragLineY(0x4C554E42, &y, ImVec4(0.72f, 0.78f, 0.86f, 0.45f),
                      1.0f, ImPlotDragToolFlags_NoInputs, nullptr, &hovered);
}

}  // namespace

void RenderChartView(std::string_view plot_id,
                     std::string_view timeframe,
                     const ChartViewSeries& series,
                     int visible_bars,
                     ChartViewDataState state,
                     const ChartViewOptions& options) {
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
    std::vector<double> opens;
    std::vector<double> highs;
    std::vector<double> lows;
    std::vector<double> closes;
    std::vector<double> volumes;
    timestamps.reserve(static_cast<std::size_t>(count));
    opens.reserve(static_cast<std::size_t>(count));
    highs.reserve(static_cast<std::size_t>(count));
    lows.reserve(static_cast<std::size_t>(count));
    closes.reserve(static_cast<std::size_t>(count));
    volumes.reserve(static_cast<std::size_t>(count));
    for (int index = first; index < static_cast<int>(series.bars.size());
         ++index) {
        const Bar& bar = series.bars[static_cast<std::size_t>(index)];
        timestamps.push_back(static_cast<double>(bar.timestamp_ms) / 1000.0);
        opens.push_back(bar.open);
        highs.push_back(bar.high);
        lows.push_back(bar.low);
        closes.push_back(bar.close);
        volumes.push_back(bar.volume);
    }

    const std::string title = "###" + std::string(plot_id);
    const float available_height = ImGui::GetContentRegionAvail().y;
    const int rows = options.show_volume ? 2 : 1;
    const ImPlotSubplotFlags subplot_flags =
        ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoLegend;
    if (!ImPlot::BeginSubplots(title.c_str(), rows, 1,
                               ImVec2(-1, std::max(220.0f, available_height)),
                               subplot_flags))
        return;
    if (ImPlot::BeginPlot("##price", ImVec2(0, 0), ImPlotFlags_NoLegend)) {
    ImPlot::SetupAxes("Time", "Price", ImPlotAxisFlags_NoMenus,
                      ImPlotAxisFlags_NoMenus);
    ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
    ImPlot::SetupAxisFormat(ImAxis_X1, "%H:%M");
    ImPlot::SetupAxisFormat(ImAxis_Y1, "%.2f");
    ImPlot::SetupMouseText(ImPlotLocation_SouthEast);
    PlotCandles(timestamps, opens, highs, lows, closes);
    if (options.show_close_line)
        ImPlot::PlotLine("Close", timestamps.data(), closes.data(), count);
    if (options.show_crosshair && ImPlot::IsPlotHovered()) {
        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
        DrawCrosshair(mouse.x, mouse.y);
    }
    ImPlot::EndPlot();
    }
    if (options.show_volume &&
        ImPlot::BeginPlot("##volume", ImVec2(0, 0), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("", "Volume", ImPlotAxisFlags_NoMenus,
                          ImPlotAxisFlags_NoMenus);
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
        ImPlot::SetupAxisFormat(ImAxis_X1, "%H:%M");
        ImPlot::PlotBars("Volume", timestamps.data(), volumes.data(), count,
                         CandleWidth(timestamps) * 0.8);
        ImPlot::EndPlot();
    }
    ImPlot::EndSubplots();
}

}  // namespace tradebox::ui
