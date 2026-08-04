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

void PlotVolumeBars(const std::vector<double>& timestamps,
                    const std::vector<double>& opens,
                    const std::vector<double>& closes,
                    const std::vector<double>& volumes) {
    const double width = CandleWidth(timestamps) * 0.78;
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    for (std::size_t index = 0; index < timestamps.size(); ++index) {
        const ImU32 color = ImGui::GetColorU32(
            closes[index] >= opens[index]
                ? ImVec4(0.20f, 0.82f, 0.52f, 0.52f)
                : ImVec4(0.93f, 0.31f, 0.38f, 0.52f));
        const ImVec2 left =
            ImPlot::PlotToPixels(timestamps[index] - width * 0.5, 0.0);
        const ImVec2 right =
            ImPlot::PlotToPixels(timestamps[index] + width * 0.5,
                                 volumes[index]);
        const ImVec2 minimum(std::min(left.x, right.x),
                             std::min(left.y, right.y));
        const ImVec2 maximum(std::max(left.x, right.x),
                             std::max(left.y, right.y));
        draw_list->AddRectFilled(minimum, maximum, color);
    }
    ImPlot::PopPlotClipRect();
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

    const int count = static_cast<int>(series.bars.size());
    const int visible_count = std::min(std::max(visible_bars, 1), count);
    const int visible_first = count - visible_count;
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
    for (int index = 0; index < count; ++index) {
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
    ImPlot::PushStyleColor(ImPlotCol_PlotBg, ImVec4(0.024f, 0.030f, 0.042f, 1.0f));
    ImPlot::PushStyleColor(ImPlotCol_PlotBorder,
                           ImVec4(0.20f, 0.24f, 0.32f, 0.55f));
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid,
                           ImVec4(0.30f, 0.36f, 0.46f, 0.22f));
    ImPlot::PushStyleColor(ImPlotCol_Crosshairs,
                           ImVec4(0.72f, 0.78f, 0.86f, 0.52f));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(8.0f, 6.0f));
    ImPlot::PushStyleVar(ImPlotStyleVar_MajorGridSize, ImVec2(1.0f, 1.0f));
    ImPlot::PushStyleVar(ImPlotStyleVar_MinorAlpha, 0.0f);
    if (!ImPlot::BeginSubplots(title.c_str(), rows, 1,
                               ImVec2(-1, std::max(220.0f, available_height)),
                               subplot_flags)) {
        ImPlot::PopStyleVar(3);
        ImPlot::PopStyleColor(4);
        return;
    }
    const double candle_width = CandleWidth(timestamps);
    const double initial_x_min = timestamps[static_cast<std::size_t>(visible_first)] -
                                 candle_width * 1.5;
    const double initial_x_max = timestamps.back() + candle_width * 2.5;
    double initial_low = lows[static_cast<std::size_t>(visible_first)];
    double initial_high = highs[static_cast<std::size_t>(visible_first)];
    double initial_volume_max = 0.0;
    for (int index = visible_first; index < count; ++index) {
        const std::size_t sample = static_cast<std::size_t>(index);
        initial_low = std::min(initial_low, lows[sample]);
        initial_high = std::max(initial_high, highs[sample]);
        initial_volume_max = std::max(initial_volume_max, volumes[sample]);
    }
    const double price_padding = std::max(
        (initial_high - initial_low) * 0.08,
        std::max(std::abs(initial_high), 1.0) * 0.002);
    ImPlot::SetNextAxisLimits(ImAxis_X1, initial_x_min, initial_x_max,
                              ImPlotCond_Once);
    const ImPlotFlags price_flags =
        ImPlotFlags_NoLegend | ImPlotFlags_NoMenus |
        (options.show_crosshair ? ImPlotFlags_Crosshairs : ImPlotFlags_None);
    if (ImPlot::BeginPlot("##price", ImVec2(0, 0), price_flags)) {
    ImPlot::SetupAxes(nullptr, "Price", ImPlotAxisFlags_NoDecorations |
                      ImPlotAxisFlags_NoMenus,
                      ImPlotAxisFlags_Opposite | ImPlotAxisFlags_NoMenus);
    ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
    ImPlot::SetupAxisFormat(ImAxis_Y1, "%.2f");
    ImPlot::SetupAxisLimits(ImAxis_Y1, initial_low - price_padding,
                            initial_high + price_padding, ImPlotCond_Once);
    ImPlot::SetupMouseText(ImPlotLocation_SouthEast);
    PlotCandles(timestamps, opens, highs, lows, closes);
    if (options.show_close_line)
        ImPlot::PlotLine("Close", timestamps.data(), closes.data(), count);
    const ImVec4 last_price_color = closes.back() >= opens.back()
        ? ImVec4(0.20f, 0.82f, 0.52f, 1.0f)
        : ImVec4(0.93f, 0.31f, 0.38f, 1.0f);
    ImPlot::TagY(closes.back(), last_price_color, "%.2f", closes.back());
    ImPlot::EndPlot();
    }
    ImPlot::SetNextAxisLimits(ImAxis_X1, initial_x_min, initial_x_max,
                              ImPlotCond_Once);
    if (options.show_volume &&
        ImPlot::BeginPlot("##volume", ImVec2(0, 0),
                          ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, "Volume", ImPlotAxisFlags_NoMenus,
                          ImPlotAxisFlags_Opposite | ImPlotAxisFlags_NoMenus);
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
        ImPlot::SetupAxisFormat(ImAxis_X1, "%H:%M");
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%.0f");
        ImPlot::SetupAxisLimits(
            ImAxis_Y1, 0.0, std::max(initial_volume_max * 1.12, 1.0),
            ImPlotCond_Once);
        PlotVolumeBars(timestamps, opens, closes, volumes);
        ImPlot::EndPlot();
    }
    ImPlot::EndSubplots();
    ImPlot::PopStyleVar(3);
    ImPlot::PopStyleColor(4);
}

}  // namespace tradebox::ui
