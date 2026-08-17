#include "chart_window.h"

#include "tradebox/workstation/asset_preferences.h"

#include "chart_geometry.h"
#include "tradebox/application/chart_query.h"
#include "tradebox/workstation/chart_documents.h"
#include "tradebox/workstation/stable_id.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

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
    persistent_changed_ = false;
    for (ChartDocumentState& chart : state.charts) {
        const auto window = state.windows.find(chart.id);
        if (window == state.windows.end() || !window->second.open)
            continue;
        static_cast<void>(Interaction(chart));
        const auto& interaction = interactions_.at(chart.id);
        if (query.asset_search.empty() && interaction.ticker[0] != '\0')
            query.asset_search = interaction.ticker.data();
        if (query.asset_search.empty() && !chart.ticker_input.empty())
            query.asset_search = chart.ticker_input;
        if (chart.instrument_id.empty() || chart.symbol.empty()) continue;
        InteractionState& mutable_interaction = Interaction(chart);
        mutable_interaction.effective_anchor_ns =
            chart.range_anchor_ns == 0 ? now_ns : chart.range_anchor_ns;
        application::ChartViewportIntent intent{
            .document_id = chart.id,
            .key = SeriesKey(chart),
            .symbol = chart.symbol,
            .anchor_ns = mutable_interaction.effective_anchor_ns,
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
        query.charts.push_back(std::move(chart_query));
    }
    query.asset_limit = query.asset_search.empty() ? 0 : 8;
    return query;
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
    ImGui::SameLine();
    if (ImGui::Button("Latest") && chart.range_anchor_ns != 0) {
        const auto applied = edit_history_.Execute(
            state, workstation::SetChartViewportCommand{
                       .document_id = chart.id,
                       .visible_bars = chart.visible_bars,
                       .range_anchor_ns = 0});
        if (applied) changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!edit_history_.CanUndo());
    if (ImGui::Button("Undo")) {
        if (edit_history_.Undo(state)) changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!edit_history_.CanRedo());
    if (ImGui::Button("Redo")) {
        if (edit_history_.Redo(state)) changed = true;
    }
    ImGui::EndDisabled();

    if (ImGui::Button("Add SMA")) {
        workstation::ChartIndicatorState indicator{
            .definition = {
                .id = workstation::NewStableId("indicator"),
                .calculation = core::SimpleMovingAverageCalculation{
                    .period = 20}},
            .label = "SMA 20",
        };
        const auto applied = edit_history_.Execute(
            state, workstation::AddChartIndicatorCommand{
                       .document_id = chart.id,
                       .indicator = std::move(indicator)});
        if (applied) changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Add EMA")) {
        workstation::ChartIndicatorState indicator{
            .definition = {
                .id = workstation::NewStableId("indicator"),
                .calculation = core::ExponentialMovingAverageCalculation{
                    .period = 20}},
            .label = "EMA 20",
            .color_rgba = 0xe0a84affU,
        };
        const auto applied = edit_history_.Execute(
            state, workstation::AddChartIndicatorCommand{
                       .document_id = chart.id,
                       .indicator = std::move(indicator)});
        if (applied) changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("H Line")) {
        interaction.drawing_tool =
            workstation::ChartDrawingKind::HorizontalLine;
        interaction.drawing_first.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("V Line")) {
        interaction.drawing_tool = workstation::ChartDrawingKind::VerticalLine;
        interaction.drawing_first.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("Trend")) {
        interaction.drawing_tool = workstation::ChartDrawingKind::TrendLine;
        interaction.drawing_first.reset();
    }
    if (interaction.drawing_tool) {
        ImGui::SameLine();
        ImGui::TextDisabled("Drawing: click chart (Esc cancels)");
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            interaction.drawing_tool.reset();
            interaction.drawing_first.reset();
        }
    }
    for (std::size_t index = 0; index < chart.indicators.size(); ++index) {
        const workstation::ChartIndicatorState original =
            chart.indicators[index];
        workstation::ChartIndicatorState edited = original;
        ImGui::PushID(original.definition.id.c_str());
        bool indicator_changed =
            ImGui::Checkbox("##visible", &edited.visible);
        ImGui::SameLine();
        ImGui::TextUnformatted(edited.label.empty()
                                   ? edited.definition.id.c_str()
                                   : edited.label.c_str());
        ImGui::SameLine();
        int period = std::visit(
            [](const auto& calculation) {
                return static_cast<int>(calculation.period);
            },
            edited.definition.calculation);
        ImGui::SetNextItemWidth(90.0F);
        if (ImGui::DragInt("Period", &period, 1.0F, 1, 10'000)) {
            std::visit(
                [period](auto& calculation) {
                    calculation.period = static_cast<std::uint32_t>(period);
                },
                edited.definition.calculation);
            edited.label = std::visit(
                [period](const auto& calculation) {
                    using T = std::decay_t<decltype(calculation)>;
                    const char* name =
                        std::is_same_v<T, core::SimpleMovingAverageCalculation>
                            ? "SMA "
                            : "EMA ";
                    return std::string(name) + std::to_string(period);
                },
                edited.definition.calculation);
            indicator_changed = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0F);
        indicator_changed =
            ImGui::SliderFloat("Width", &edited.line_width, 0.5F, 10.0F) ||
            indicator_changed;
        ImGui::SameLine();
        const bool remove = ImGui::SmallButton("Remove");
        ImGui::PopID();
        if (remove) {
            if (edit_history_.Execute(
                    state, workstation::RemoveChartIndicatorCommand{
                               .document_id = chart.id,
                               .indicator_id = original.definition.id}))
                changed = true;
            break;
        }
        if (indicator_changed) {
            if (edit_history_.Execute(
                    state, workstation::UpdateChartIndicatorCommand{
                               .document_id = chart.id,
                               .indicator = std::move(edited)}))
                changed = true;
        }
    }
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
            workstation::RecordAssetSelection(
                state, asset.instrument_id);
            const auto count = std::min(asset.symbol.size(),
                                        interaction.ticker.size() - 1);
            std::copy_n(asset.symbol.data(), count, interaction.ticker.data());
            interaction.ticker[count] = '\0';
            interaction.ticker.back() = '\0';
            interaction.resolve_requested = false;
            persistent_changed_ = true;
        }
    } else if (snapshot == nullptr) {
        ImGui::TextUnformatted("Chart data is unavailable.");
    } else {
        ImGui::SameLine();
        ImGui::Text("%s", StatusLabel(snapshot->status));
        if (!snapshot->message.empty())
            ImGui::TextWrapped("%s", snapshot->message.c_str());
        if (snapshot->status == application::ChartDataStatus::Ready ||
            snapshot->status == application::ChartDataStatus::Empty ||
            snapshot->status == application::ChartDataStatus::MissingHistory ||
            snapshot->status == application::ChartDataStatus::Loading)
            DrawSeries(*snapshot, state, chart, interaction);
    }
    ImGui::PopID();
    workspace.EndWindow(window);
}

void ChartWindowRenderer::DrawSeries(
    const application::UiChartSnapshot& snapshot,
    workstation::WorkspaceState& state, ChartDocumentState& chart_state,
    InteractionState& interaction) {
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
    const auto& series = snapshot.series;
    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (const auto zoomed = chart::ZoomViewport(
                {.visible_bars = chart_state.visible_bars,
                 .anchor_ns = interaction.effective_anchor_ns},
                series.requested_range, ImGui::GetIO().MousePos.x, plot,
                wheel)) {
            const auto applied = edit_history_.Execute(
                state, workstation::SetChartViewportCommand{
                           .document_id = chart_state.id,
                           .visible_bars = zoomed->visible_bars,
                           .range_anchor_ns = zoomed->anchor_ns});
            if (applied) persistent_changed_ = true;
        }
    }
    if (ImGui::IsItemActivated() && !interaction.drawing_tool) {
        interaction.panning = true;
        interaction.pan_origin_x = ImGui::GetIO().MousePos.x;
        interaction.pan_origin_anchor_ns = interaction.effective_anchor_ns;
        interaction.preview_anchor_ns = interaction.effective_anchor_ns;
    }
    if (interaction.panning && ImGui::IsItemActive() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (const auto anchor = chart::PanViewportAnchor(
                interaction.pan_origin_anchor_ns, series.requested_range,
                ImGui::GetIO().MousePos.x - interaction.pan_origin_x, plot))
            interaction.preview_anchor_ns = *anchor;
    }
    if (interaction.panning && ImGui::IsItemDeactivated()) {
        if (interaction.preview_anchor_ns !=
            interaction.pan_origin_anchor_ns) {
            const auto applied = edit_history_.Execute(
                state, workstation::SetChartViewportCommand{
                           .document_id = chart_state.id,
                           .visible_bars = chart_state.visible_bars,
                           .range_anchor_ns =
                               interaction.preview_anchor_ns});
            if (applied) persistent_changed_ = true;
        }
        interaction.panning = false;
    }
    core::BarRange rendered_range = series.requested_range;
    if (interaction.panning) {
        rendered_range = chart::ShiftRange(
            rendered_range,
            interaction.preview_anchor_ns -
                interaction.pan_origin_anchor_ns);
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({plot.left, plot.top}, {plot.right, plot.bottom},
                        ImGui::GetColorU32(ImVec4{0.035f, 0.045f, 0.065f, 1.0f}));
    if (chart_state.show_volume)
        draw->AddRectFilled({volume_plot.left, volume_plot.top},
                            {volume_plot.right, volume_plot.bottom},
                            ImGui::GetColorU32(ImVec4{0.025f, 0.035f, 0.050f, 1.0f}));
    const auto visible = chart::SelectVisibleIndices(series.bars,
                                                       rendered_range);
    std::vector<core::MarketBar> aggregated_bars;
    std::span<const core::MarketBar> render_bars = series.bars;
    chart::VisibleIndices render_visible = visible;
    if (!visible.Empty() &&
        visible.last - visible.first >
            static_cast<std::size_t>(std::max(1.0F, plot.Width()))) {
        aggregated_bars = chart::AggregateBarsByScreenColumn(
            series.bars, visible, rendered_range, plot);
        render_bars = aggregated_bars;
        render_visible = {.first = 0, .last = aggregated_bars.size()};
    }
    const core::MarketBar* current =
        series.current_bar ? &*series.current_bar : nullptr;
    const core::MarketBar* visible_current =
        current != nullptr && current->start_ns >= rendered_range.start_ns &&
                current->start_ns < rendered_range.end_ns
            ? current
            : nullptr;
    const auto scale = chart::AutoscalePrices(
        series.bars, visible, visible_current);
    if (!scale) {
        draw->AddText({plot.left + 12.0f, plot.top + 12.0f},
                      ImGui::GetColorU32(ImGuiCol_TextDisabled),
                      "No bars in this range");
        return;
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto mouse_anchor = [&]() {
        const auto price = core::Decimal::Parse(
            std::to_string(scale->FromY(mouse.y, plot)));
        return workstation::ChartDrawingAnchorState{
            .time_ns = chart::XToTime(mouse.x, rendered_range, plot),
            .price = price ? *price : core::Decimal::Zero()};
    };
    if (interaction.drawing_tool && ImGui::IsItemClicked() &&
        !chart_state.instrument_id.empty() && mouse.x >= plot.left &&
        mouse.x <= plot.right && mouse.y >= plot.top &&
        mouse.y <= plot.bottom) {
        const workstation::ChartDrawingAnchorState anchor = mouse_anchor();
        const bool needs_second =
            *interaction.drawing_tool ==
            workstation::ChartDrawingKind::TrendLine;
        if (needs_second && !interaction.drawing_first) {
            interaction.drawing_first = anchor;
        } else {
            workstation::ChartDrawingState drawing{
                .id = workstation::NewStableId("drawing"),
                .instrument_id = chart_state.instrument_id,
                .kind = *interaction.drawing_tool,
                .first = interaction.drawing_first.value_or(anchor),
                .second = needs_second
                              ? std::optional<
                                    workstation::ChartDrawingAnchorState>(anchor)
                              : std::nullopt,
            };
            const std::string drawing_id = drawing.id;
            if (edit_history_.Execute(
                    state, workstation::AddChartDrawingCommand{
                               .drawing = std::move(drawing)})) {
                interaction.selected_drawing_id = drawing_id;
                persistent_changed_ = true;
            }
            interaction.drawing_tool.reset();
            interaction.drawing_first.reset();
        }
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
    const std::size_t visible_count =
        render_visible.last - render_visible.first;
    const float body_half_width = visible_count == 0
                                      ? 2.0f
                                      : std::clamp(plot.Width() /
                                                       static_cast<float>(visible_count) *
                                                       0.32f,
                                                   1.0f, 8.0f);
    for (std::size_t index = render_visible.first;
         index < render_visible.last; ++index) {
        const auto candle = chart::MakeCandleGeometry(
            render_bars[index], *scale, rendered_range, plot,
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
    if (visible_current != nullptr) {
        const bool duplicate = std::ranges::any_of(
            series.bars, [visible_current](const core::MarketBar& bar) {
                return bar.start_ns == visible_current->start_ns;
            });
        if (!duplicate) {
            const auto candle = chart::MakeCandleGeometry(
                *visible_current, *scale, rendered_range, plot,
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
    if (snapshot.indicator_projection) {
        for (const core::IndicatorSeries& indicator_series :
             snapshot.indicator_projection->series) {
            const auto configured = std::ranges::find(
                chart_state.indicators, indicator_series.indicator_id,
                [](const workstation::ChartIndicatorState& value) {
                    return value.definition.id;
                });
            if (configured == chart_state.indicators.end() ||
                !configured->visible)
                continue;
            const std::uint32_t rgba = configured->color_rgba;
            const ImVec4 color{
                static_cast<float>((rgba >> 24U) & 0xffU) / 255.0F,
                static_cast<float>((rgba >> 16U) & 0xffU) / 255.0F,
                static_cast<float>((rgba >> 8U) & 0xffU) / 255.0F,
                static_cast<float>(rgba & 0xffU) / 255.0F};
            const auto first = std::ranges::lower_bound(
                indicator_series.points, rendered_range.start_ns, {},
                &core::IndicatorPoint::start_ns);
            const auto last = std::ranges::lower_bound(
                indicator_series.points, rendered_range.end_ns, {},
                &core::IndicatorPoint::start_ns);
            if (first == last) continue;
            auto previous = first;
            for (auto point = std::next(first); point != last;
                 ++point, ++previous) {
                draw->AddLine(
                    {chart::TimeToX(previous->start_ns, rendered_range, plot),
                     scale->ToY(previous->value.ToDisplayDouble(), plot)},
                    {chart::TimeToX(point->start_ns, rendered_range, plot),
                     scale->ToY(point->value.ToDisplayDouble(), plot)},
                    ImGui::GetColorU32(color), configured->line_width);
            }
        }
    }
    const auto drawing_color = [](std::uint32_t rgba) {
        return ImVec4{
            static_cast<float>((rgba >> 24U) & 0xffU) / 255.0F,
            static_cast<float>((rgba >> 16U) & 0xffU) / 255.0F,
            static_cast<float>((rgba >> 8U) & 0xffU) / 255.0F,
            static_cast<float>(rgba & 0xffU) / 255.0F};
    };
    const auto draw_drawing = [&](const workstation::ChartDrawingState& value,
                                  bool selected) {
        const float first_x = chart::TimeToX(
            value.first.time_ns, rendered_range, plot);
        const float first_y = scale->ToY(
            value.first.price.ToDisplayDouble(), plot);
        const ImU32 color = ImGui::GetColorU32(
            selected ? ImVec4{1.0F, 0.82F, 0.25F, 1.0F}
                     : drawing_color(value.color_rgba));
        switch (value.kind) {
            case workstation::ChartDrawingKind::HorizontalLine:
                draw->AddLine({plot.left, first_y}, {plot.right, first_y},
                              color, value.line_width);
                break;
            case workstation::ChartDrawingKind::VerticalLine:
                draw->AddLine({first_x, plot.top}, {first_x, plot.bottom},
                              color, value.line_width);
                break;
            case workstation::ChartDrawingKind::TrendLine:
            case workstation::ChartDrawingKind::Ray:
                if (value.second) {
                    draw->AddLine(
                        {first_x, first_y},
                        {chart::TimeToX(value.second->time_ns, rendered_range,
                                        plot),
                         scale->ToY(value.second->price.ToDisplayDouble(),
                                    plot)},
                        color, value.line_width);
                }
                break;
            case workstation::ChartDrawingKind::Rectangle:
                if (value.second) {
                    draw->AddRect(
                        {first_x, first_y},
                        {chart::TimeToX(value.second->time_ns, rendered_range,
                                        plot),
                         scale->ToY(value.second->price.ToDisplayDouble(),
                                    plot)},
                        color, 0.0F, 0, value.line_width);
                }
                break;
        }
    };
    for (const workstation::ChartDrawingState& drawing : state.chart_drawings) {
        if (drawing.instrument_id != chart_state.instrument_id ||
            !drawing.visible)
            continue;
        draw_drawing(drawing,
                     drawing.id == interaction.selected_drawing_id);
    }
    if (!interaction.selected_drawing_id.empty() &&
        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (edit_history_.Execute(
                state, workstation::RemoveChartDrawingCommand{
                           interaction.selected_drawing_id})) {
            interaction.selected_drawing_id.clear();
            persistent_changed_ = true;
        }
    }
    if (interaction.drawing_tool && interaction.drawing_first &&
        mouse.x >= plot.left && mouse.x <= plot.right &&
        mouse.y >= plot.top && mouse.y <= plot.bottom) {
        workstation::ChartDrawingState preview{
            .instrument_id = chart_state.instrument_id,
            .kind = *interaction.drawing_tool,
            .first = *interaction.drawing_first,
            .second = mouse_anchor(),
            .color_rgba = 0xffffff99U,
        };
        draw_drawing(preview, true);
    }
    if (visible_current != nullptr) {
        const float current_y = scale->ToY(
            visible_current->close.ToDisplayDouble(), plot);
        draw->AddLine({plot.left, current_y}, {plot.right, current_y},
                      ImGui::GetColorU32(kFlatColor));
        char current_label[48]{};
        std::snprintf(current_label, sizeof(current_label), "last %.2f",
                      visible_current->close.ToDisplayDouble());
        draw->AddText({plot.left + 8.0f, current_y - 18.0f},
                      ImGui::GetColorU32(kFlatColor), current_label);
    }
    draw->PopClipRect();

    if (chart_state.show_volume) {
        const auto maximum_volume = chart::MaximumVolume(
            render_bars, render_visible, visible_current);
        if (maximum_volume) {
            draw->PushClipRect({volume_plot.left, volume_plot.top},
                               {volume_plot.right, volume_plot.bottom}, true);
            for (std::size_t index = render_visible.first;
                 index < render_visible.last;
                 ++index) {
                const auto candle = chart::MakeCandleGeometry(
                    render_bars[index], *scale, rendered_range, plot,
                    body_half_width);
                const float volume_y = chart::VolumeToY(
                    render_bars[index].volume.ToDisplayDouble(),
                    *maximum_volume, volume_plot);
                draw->AddRectFilled(
                    {candle.x - candle.body_half_width, volume_y},
                    {candle.x + candle.body_half_width, volume_plot.bottom},
                    ImGui::GetColorU32(kVolumeColor));
            }
            if (visible_current != nullptr) {
                const bool duplicate = std::ranges::any_of(
                    series.bars, [visible_current](const core::MarketBar& bar) {
                        return bar.start_ns == visible_current->start_ns;
                    });
                if (!duplicate) {
                    const auto candle = chart::MakeCandleGeometry(
                        *visible_current, *scale, rendered_range, plot,
                        body_half_width);
                    const float volume_y = chart::VolumeToY(
                        visible_current->volume.ToDisplayDouble(),
                        *maximum_volume,
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
        const ImVec2 crosshair_mouse = ImGui::GetIO().MousePos;
        if (crosshair_mouse.x >= plot.left &&
            crosshair_mouse.x <= plot.right &&
            crosshair_mouse.y >= plot.top &&
            crosshair_mouse.y <= plot.bottom) {
            draw->AddLine({crosshair_mouse.x, plot.top},
                          {crosshair_mouse.x, plot.bottom},
                          ImGui::GetColorU32(ImGuiCol_TextDisabled));
            draw->AddLine({plot.left, crosshair_mouse.y},
                          {plot.right, crosshair_mouse.y},
                          ImGui::GetColorU32(ImGuiCol_TextDisabled));
            const std::int64_t time = chart::XToTime(
                crosshair_mouse.x, rendered_range, plot);
            const auto hit = chart::HitTestBar(
                series.bars, visible, time, rendered_range);
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

bool ChartWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

}  // namespace tradebox::gui
