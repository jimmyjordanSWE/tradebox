#include "chart_window.h"

#include "chart_geometry.h"
#include "tradebox/application/chart_query.h"
#include "tradebox/workstation/chart_documents.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>

namespace tradebox::gui {
namespace {

using workstation::ChartDocumentState;

constexpr ImVec4 kUpColor{0.22f, 0.75f, 0.45f, 1.0f};
constexpr ImVec4 kDownColor{0.92f, 0.32f, 0.35f, 1.0f};
constexpr ImVec4 kFlatColor{0.65f, 0.68f, 0.72f, 1.0f};
constexpr ImVec4 kVolumeColor{0.28f, 0.52f, 0.78f, 0.45f};

core::BarSeriesKey SeriesKey(const ChartDocumentState& chart) {
    return {
        .instrument_id = chart.instrument_id,
        .feed = chart.feed,
        .timeframe = chart.timeframe,
        .adjustment = chart.adjustment,
    };
}

const char* StatusLabel(application::ChartDataStatus status) {
    switch (status) {
        case application::ChartDataStatus::Unavailable: return "Unavailable";
        case application::ChartDataStatus::Loading: return "Loading";
        case application::ChartDataStatus::Ready: return "Ready";
        case application::ChartDataStatus::Empty: return "No data";
        case application::ChartDataStatus::MissingHistory: return "Missing history";
        case application::ChartDataStatus::Failed: return "History failed";
    }
    return "Unknown";
}

void MarkChanged(bool& changed, bool value) { changed = changed || value; }

bool SameSymbol(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto left_char = static_cast<unsigned char>(left[index]);
        const auto right_char = static_cast<unsigned char>(right[index]);
        if (std::toupper(left_char) != std::toupper(right_char)) return false;
    }
    return true;
}

}  // namespace

void DrawSeries(const application::UiChartSnapshot& snapshot,
                const workstation::ChartDocumentState& chart_state);

ChartWindowRenderer::InteractionState& ChartWindowRenderer::Interaction(
    const ChartDocumentState& chart) {
    auto& interaction = interactions_[chart.id];
    if (!interaction.initialized) {
        const auto count = std::min(chart.ticker_input.size(),
                                    interaction.ticker.size() - 1);
        std::copy_n(chart.ticker_input.data(), count, interaction.ticker.data());
        interaction.ticker[count] = '\0';
        interaction.ticker.back() = '\0';
        interaction.initialized = true;
    }
    return interaction;
}

application::UiSnapshotQuery ChartWindowRenderer::BuildSnapshotQuery(
    workstation::WorkspaceState& state, std::int64_t now_ns) {
    application::UiSnapshotQuery query;
    queries_.clear();
    persistent_changed_ = false;
    for (ChartDocumentState& chart : state.charts) {
        static_cast<void>(Interaction(chart));
        const auto& interaction = interactions_.at(chart.id);
        if (query.asset_search.empty() && interaction.ticker[0] != '\0')
            query.asset_search = interaction.ticker.data();
        if (query.asset_search.empty() && !chart.ticker_input.empty())
            query.asset_search = chart.ticker_input;
        if (chart.instrument_id.empty() || chart.symbol.empty()) continue;
        if (chart.range_anchor_ns == 0 && now_ns > 0) {
            chart.range_anchor_ns = now_ns;
            persistent_changed_ = true;
        }
        application::ChartViewportIntent intent{
            .document_id = chart.id,
            .key = SeriesKey(chart),
            .symbol = chart.symbol,
            .anchor_ns = chart.range_anchor_ns,
            .visible_bars = static_cast<std::size_t>(chart.visible_bars),
        };
        const auto range = application::ResolveChartRange(intent);
        if (!range) continue;
        application::UiChartQuery chart_query{
            .document_id = chart.id,
            .key = intent.key,
            .symbol = chart.symbol,
            .range = *range,
        };
        chart_query.indicators.reserve(chart.indicators.size());
        for (const auto& indicator : chart.indicators)
            chart_query.indicators.push_back(indicator.definition);
        queries_.insert_or_assign(chart.id, chart_query);
        query.charts.push_back(std::move(chart_query));
    }
    query.asset_limit = query.asset_search.empty() ? 0 : 8;
    return query;
}

void ChartWindowRenderer::RequestMissingHistory(
    application::TradingApplication& application,
    const application::ApplicationUiSnapshot& snapshot) {
    for (const auto& chart : snapshot.charts) {
        if (chart.status != application::ChartDataStatus::MissingHistory)
            continue;
        const auto query = queries_.find(chart.document_id);
        if (query == queries_.end()) continue;
        auto& interaction = interactions_[chart.document_id];
        if (interaction.has_requested_range &&
            interaction.requested_key == query->second.key &&
            interaction.requested_range == query->second.range)
            continue;
        application.RequestMarketHistory(query->second);
        interaction.has_requested_range = true;
        interaction.requested_key = query->second.key;
        interaction.requested_range = query->second.range;
    }
}

const application::UiChartSnapshot* ChartWindowRenderer::SnapshotFor(
    std::string_view document_id,
    const application::ApplicationUiSnapshot& snapshot) const {
    const auto found = std::ranges::find(
        snapshot.charts, document_id,
        &application::UiChartSnapshot::document_id);
    return found == snapshot.charts.end() ? nullptr : &*found;
}

void ChartWindowRenderer::Draw(
    ui::Workspace& workspace, workstation::WorkspaceState& state,
    const application::ApplicationUiSnapshot& snapshot) {
    for (ChartDocumentState& chart : state.charts) {
        const auto window = state.windows.find(chart.id);
        if (window == state.windows.end() || !window->second.open) continue;
        DrawChartWindow(workspace, state, chart,
                        SnapshotFor(chart.id, snapshot), snapshot);
    }
}

void ChartWindowRenderer::DrawChartWindow(
    ui::Workspace& workspace, workstation::WorkspaceState& state,
    ChartDocumentState& chart,
    const application::UiChartSnapshot* snapshot,
    const application::ApplicationUiSnapshot& app_snapshot) {
    ui::WorkspaceWindow window{
        .title = chart.symbol.empty()
                     ? "Chart"
                     : chart.symbol + " - " + chart.timeframe,
        .id = chart.id,
        .default_offset = {48.0f, 48.0f},
        .default_size = {960.0f, 640.0f},
        .open = true,
    };
    workspace.ConstrainNextWindowSize({640.0f, 420.0f});
    const bool visible = workspace.BeginWindow(window);
    if (!visible) {
        workspace.EndWindow(window);
        return;
    }

    ImGui::PushID(chart.id.c_str());
    InteractionState& interaction = Interaction(chart);
    bool changed = false;
    ImGui::SetNextItemWidth(180.0f);
    const bool ticker_edited = ImGui::InputText(
        "Ticker", interaction.ticker.data(), interaction.ticker.size(),
        ImGuiInputTextFlags_CharsUppercase);
    const bool ticker_submitted = ImGui::IsItemFocused() &&
                                  ImGui::IsKeyPressed(ImGuiKey_Enter);
    if (ticker_edited) {
        const std::string next(interaction.ticker.data());
        if (chart.ticker_input != next) {
            chart.ticker_input = next;
            changed = true;
        }
        interaction.resolve_requested = false;
    }
    if (ticker_submitted) interaction.resolve_requested = true;
    ImGui::SameLine();
    if (ImGui::BeginCombo("Timeframe", chart.timeframe.c_str())) {
        constexpr const char* options[] = {
            "1Min", "5Min", "15Min", "1Hour", "1Day"};
        for (const char* option : options) {
            const bool selected = chart.timeframe == option;
            if (ImGui::Selectable(option, selected)) {
                chart.timeframe = option;
                chart.range_anchor_ns = 0;
                interaction.has_requested_range = false;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    MarkChanged(changed, ImGui::Checkbox("Volume", &chart.show_volume));
    ImGui::SameLine();
    MarkChanged(changed, ImGui::Checkbox("Crosshair", &chart.show_crosshair));
    if (changed) persistent_changed_ = true;

    if (chart.instrument_id.empty()) {
        ImGui::TextUnformatted(
            "Enter a ticker and press Enter, or choose a result below.");
        if (interaction.resolve_requested && app_snapshot.assets.empty())
            ImGui::TextDisabled("No matching local asset was found.");
        for (const auto& asset : app_snapshot.assets) {
            const std::string label = asset.symbol + " - " + asset.name;
            const bool selected =
                ImGui::Selectable(label.c_str(), false,
                                  ImGuiSelectableFlags_AllowDoubleClick);
            const bool submitted_match =
                interaction.resolve_requested &&
                SameSymbol(asset.symbol, interaction.ticker.data());
            if (!selected && !submitted_match) continue;
            const auto assigned = workstation::AssignChartInstrument(
                state, chart.id, asset.instrument_id, asset.symbol);
            if (!assigned) continue;
            const auto count = std::min(asset.symbol.size(),
                                        interaction.ticker.size() - 1);
            std::copy_n(asset.symbol.data(), count, interaction.ticker.data());
            interaction.ticker[count] = '\0';
            interaction.ticker.back() = '\0';
            interaction.resolve_requested = false;
            interaction.has_requested_range = false;
            persistent_changed_ = true;
        }
    } else if (snapshot == nullptr) {
        ImGui::TextUnformatted("Chart data is unavailable.");
    } else {
        ImGui::SameLine();
        ImGui::Text("%s", StatusLabel(snapshot->status));
        if (!snapshot->message.empty())
            ImGui::TextWrapped("%s", snapshot->message.c_str());
        if (snapshot->status == application::ChartDataStatus::Failed &&
            ImGui::Button("Retry history")) {
            const auto query = queries_.find(chart.id);
            if (query != queries_.end()) retries_.push_back(query->second);
        }
        if (snapshot->status == application::ChartDataStatus::Ready ||
            snapshot->status == application::ChartDataStatus::Empty ||
            snapshot->status == application::ChartDataStatus::MissingHistory ||
            snapshot->status == application::ChartDataStatus::Loading)
            DrawSeries(*snapshot, chart);
    }
    ImGui::PopID();
    workspace.EndWindow(window);
}

void DrawSeries(const application::UiChartSnapshot& snapshot,
                const ChartDocumentState& chart_state) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float width = std::max(1.0f, available.x);
    const float height = std::max(1.0f, available.y);
    ImGui::InvisibleButton("##chart_canvas", {width, height});
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const float volume_height =
        chart_state.show_volume ? std::max(56.0f, height * 0.22f) : 0.0f;
    const chart::Rect plot{
        minimum.x + 8.0f, minimum.y + 8.0f,
        maximum.x - 58.0f, maximum.y - volume_height - 24.0f};
    const chart::Rect volume_plot{
        plot.left, plot.bottom + 12.0f, plot.right, maximum.y - 20.0f};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({plot.left, plot.top}, {plot.right, plot.bottom},
                        ImGui::GetColorU32(ImVec4{0.035f, 0.045f, 0.065f, 1.0f}));
    if (chart_state.show_volume)
        draw->AddRectFilled({volume_plot.left, volume_plot.top},
                            {volume_plot.right, volume_plot.bottom},
                            ImGui::GetColorU32(ImVec4{0.025f, 0.035f, 0.050f, 1.0f}));
    const auto& series = snapshot.series;
    const auto visible = chart::SelectVisibleIndices(series.bars,
                                                       series.requested_range);
    const core::MarketBar* current =
        series.current_bar ? &*series.current_bar : nullptr;
    const auto scale = chart::AutoscalePrices(
        series.bars, visible, current);
    if (!scale) {
        draw->AddText({plot.left + 12.0f, plot.top + 12.0f},
                      ImGui::GetColorU32(ImGuiCol_TextDisabled),
                      "No bars in this range");
        return;
    }
    for (int line = 0; line <= 4; ++line) {
        const float y = plot.top + plot.Height() *
                                      static_cast<float>(line) / 4.0f;
        draw->AddLine({plot.left, y}, {plot.right, y},
                      ImGui::GetColorU32(ImGuiCol_Border));
        const double value = scale->FromY(y, plot);
        char label[32]{};
        std::snprintf(label, sizeof(label), "%.2f", value);
        draw->AddText({plot.right + 6.0f, y - 7.0f},
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    }
    draw->PushClipRect({plot.left, plot.top}, {plot.right, plot.bottom}, true);
    const std::size_t visible_count = visible.last - visible.first;
    const float body_half_width = visible_count == 0
                                      ? 2.0f
                                      : std::clamp(plot.Width() /
                                                       static_cast<float>(visible_count) *
                                                       0.32f,
                                                   1.0f, 8.0f);
    for (std::size_t index = visible.first; index < visible.last; ++index) {
        const auto candle = chart::MakeCandleGeometry(
            series.bars[index], *scale, series.requested_range, plot,
            body_half_width);
        const ImVec4 color = candle.unchanged
                                 ? kFlatColor
                                 : candle.rising ? kUpColor : kDownColor;
        draw->AddLine({candle.x, candle.high_y},
                      {candle.x, candle.low_y}, ImGui::GetColorU32(color));
        draw->AddRectFilled(
            {candle.x - candle.body_half_width,
             std::min(candle.open_y, candle.close_y)},
            {candle.x + candle.body_half_width,
             std::max(candle.open_y, candle.close_y)},
            ImGui::GetColorU32(color));
    }
    if (current != nullptr) {
        const bool duplicate = std::ranges::any_of(
            series.bars, [current](const core::MarketBar& bar) {
                return bar.start_ns == current->start_ns;
            });
        if (!duplicate && current->start_ns >= series.requested_range.start_ns &&
            current->start_ns < series.requested_range.end_ns) {
            const auto candle = chart::MakeCandleGeometry(
                *current, *scale, series.requested_range, plot,
                body_half_width);
            const ImVec4 color = candle.unchanged
                                     ? kFlatColor
                                     : candle.rising ? kUpColor : kDownColor;
            draw->AddLine({candle.x, candle.high_y},
                          {candle.x, candle.low_y}, ImGui::GetColorU32(color));
            draw->AddRectFilled(
                {candle.x - candle.body_half_width,
                 std::min(candle.open_y, candle.close_y)},
                {candle.x + candle.body_half_width,
                 std::max(candle.open_y, candle.close_y)},
                ImGui::GetColorU32(color));
        }
    }
    if (current != nullptr) {
        const float current_y = scale->ToY(
            current->close.ToDisplayDouble(), plot);
        draw->AddLine({plot.left, current_y}, {plot.right, current_y},
                      ImGui::GetColorU32(kFlatColor));
        char current_label[48]{};
        std::snprintf(current_label, sizeof(current_label), "last %.2f",
                      current->close.ToDisplayDouble());
        draw->AddText({plot.left + 8.0f, current_y - 18.0f},
                      ImGui::GetColorU32(kFlatColor), current_label);
    }
    draw->PopClipRect();

    if (chart_state.show_volume) {
        const auto maximum_volume = chart::MaximumVolume(
            series.bars, visible, current);
        if (maximum_volume) {
            draw->PushClipRect({volume_plot.left, volume_plot.top},
                               {volume_plot.right, volume_plot.bottom}, true);
            for (std::size_t index = visible.first; index < visible.last;
                 ++index) {
                const auto candle = chart::MakeCandleGeometry(
                    series.bars[index], *scale, series.requested_range, plot,
                    body_half_width);
                const float volume_y = chart::VolumeToY(
                    series.bars[index].volume.ToDisplayDouble(),
                    *maximum_volume, volume_plot);
                draw->AddRectFilled(
                    {candle.x - candle.body_half_width, volume_y},
                    {candle.x + candle.body_half_width, volume_plot.bottom},
                    ImGui::GetColorU32(kVolumeColor));
            }
            if (current != nullptr) {
                const bool duplicate = std::ranges::any_of(
                    series.bars, [current](const core::MarketBar& bar) {
                        return bar.start_ns == current->start_ns;
                    });
                if (!duplicate && current->start_ns >=
                                       series.requested_range.start_ns &&
                    current->start_ns < series.requested_range.end_ns) {
                    const auto candle = chart::MakeCandleGeometry(
                        *current, *scale, series.requested_range, plot,
                        body_half_width);
                    const float volume_y = chart::VolumeToY(
                        current->volume.ToDisplayDouble(), *maximum_volume,
                        volume_plot);
                    draw->AddRectFilled(
                        {candle.x - candle.body_half_width, volume_y},
                        {candle.x + candle.body_half_width, volume_plot.bottom},
                        ImGui::GetColorU32(kVolumeColor));
                }
            }
            draw->PopClipRect();
        }
    }
    if (chart_state.show_crosshair && ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (mouse.x >= plot.left && mouse.x <= plot.right &&
            mouse.y >= plot.top && mouse.y <= plot.bottom) {
            draw->AddLine({mouse.x, plot.top}, {mouse.x, plot.bottom},
                          ImGui::GetColorU32(ImGuiCol_TextDisabled));
            draw->AddLine({plot.left, mouse.y}, {plot.right, mouse.y},
                          ImGui::GetColorU32(ImGuiCol_TextDisabled));
            const std::int64_t time = chart::XToTime(
                mouse.x, series.requested_range, plot);
            const auto hit = chart::HitTestBar(
                series.bars, visible, time, series.requested_range);
            char label[96]{};
            if (hit) {
                std::snprintf(label, sizeof(label), "t=%lld  close=%.2f",
                              static_cast<long long>(
                                  series.bars[*hit].start_ns),
                              series.bars[*hit].close.ToDisplayDouble());
            } else {
                std::snprintf(label, sizeof(label), "t=%lld",
                              static_cast<long long>(time));
            }
            draw->AddText({plot.left + 8.0f, plot.top + 8.0f},
                          ImGui::GetColorU32(ImGuiCol_Text), label);
        }
    }
}

std::vector<application::UiChartQuery>
ChartWindowRenderer::ConsumeHistoryRetries() {
    std::vector<application::UiChartQuery> result;
    result.swap(retries_);
    return result;
}

bool ChartWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

}  // namespace tradebox::gui
