#include "tradebox/workstation/chart_documents.h"

#include "tradebox/workstation/stable_id.h"

#include <algorithm>
#include <map>
#include <ranges>
#include <utility>

namespace tradebox::workstation {
namespace {

std::expected<WindowInstanceState*, ChartDocumentError> ChartWindow(
    WorkspaceState& workspace, std::string_view document_id) {
    const auto found = workspace.windows.find(document_id);
    if (found == workspace.windows.end() || found->second.kind != "chart")
        return std::unexpected(ChartDocumentError{
            "chart document has no matching chart window"});
    return &found->second;
}

std::vector<ChartIndicatorState> CloneIndicators(
    const std::vector<ChartIndicatorState>& source) {
    std::vector<ChartIndicatorState> result = source;
    std::map<std::string, std::string, std::less<>> remapped_ids;
    for (ChartIndicatorState& indicator : result) {
        std::string replacement = NewStableId("indicator");
        remapped_ids.emplace(indicator.definition.id, replacement);
        indicator.definition.id = std::move(replacement);
    }
    for (ChartIndicatorState& indicator : result) {
        core::IndicatorInput& input = std::visit(
            [](auto& calculation) -> core::IndicatorInput& {
                return calculation.input;
            },
            indicator.definition.calculation);
        auto* reference =
            std::get_if<core::IndicatorOutputInput>(&input);
        if (reference == nullptr) continue;
        const auto remapped = remapped_ids.find(reference->indicator_id);
        if (remapped != remapped_ids.end())
            reference->indicator_id = remapped->second;
    }
    return result;
}

}  // namespace

ChartDocumentState* FindChartDocument(
    WorkspaceState& workspace, std::string_view document_id) {
    const auto found = std::ranges::find(
        workspace.charts, document_id, &ChartDocumentState::id);
    return found == workspace.charts.end() ? nullptr : &*found;
}

const ChartDocumentState* FindChartDocument(
    const WorkspaceState& workspace, std::string_view document_id) {
    const auto found = std::ranges::find(
        workspace.charts, document_id, &ChartDocumentState::id);
    return found == workspace.charts.end() ? nullptr : &*found;
}

std::expected<std::string, ChartDocumentError> CreateChartDocument(
    WorkspaceState& workspace, CreateChartDocumentRequest request) {
    if (request.instrument_id.empty() != request.symbol.empty())
        return std::unexpected(ChartDocumentError{
            "chart instrument identity and symbol must be assigned together"});

    std::string id;
    do {
        id = NewStableId("chart");
    } while (FindChartDocument(workspace, id) != nullptr ||
             workspace.windows.contains(id));

    const ChartDefaultsState& defaults = workspace.chart_defaults;
    workspace.charts.push_back({
        .id = id,
        .instrument_id = std::move(request.instrument_id),
        .symbol = request.symbol,
        .ticker_input = std::move(request.symbol),
        .timeframe = defaults.timeframe,
        .feed = defaults.feed,
        .adjustment = defaults.adjustment,
        .visible_bars = defaults.visible_bars,
        .show_volume = defaults.show_volume,
        .show_close_line = defaults.show_close_line,
        .show_crosshair = defaults.show_crosshair,
        .indicators = CloneIndicators(defaults.indicators),
    });
    const ChartDocumentState& chart = workspace.charts.back();
    const float cascade =
        static_cast<float>((workspace.charts.size() - 1U) % 8U) * 24.0f;
    workspace.windows.emplace(
        id,
        WindowInstanceState{
            .id = id,
            .kind = "chart",
            .title = chart.symbol.empty()
                         ? "Chart"
                         : chart.symbol + " · " + chart.timeframe,
            .open = true,
            .bounds = {48.0f + cascade, 48.0f + cascade, 960.0f, 640.0f},
        });
    return id;
}

std::expected<void, ChartDocumentError> AssignChartInstrument(
    WorkspaceState& workspace, std::string_view document_id,
    std::string instrument_id, std::string symbol) {
    if (instrument_id.empty() || symbol.empty())
        return std::unexpected(ChartDocumentError{
            "chart assignment requires stable instrument identity and symbol"});
    ChartDocumentState* chart = FindChartDocument(workspace, document_id);
    if (chart == nullptr)
        return std::unexpected(
            ChartDocumentError{"chart document does not exist"});
    auto window = ChartWindow(workspace, document_id);
    if (!window) return std::unexpected(window.error());
    chart->instrument_id = std::move(instrument_id);
    chart->symbol = std::move(symbol);
    chart->ticker_input = chart->symbol;
    chart->range_anchor_ns = 0;
    (*window)->title = chart->symbol + " · " + chart->timeframe;
    return {};
}

std::expected<void, ChartDocumentError> CloseChartDocument(
    WorkspaceState& workspace, std::string_view document_id) {
    if (FindChartDocument(workspace, document_id) == nullptr)
        return std::unexpected(
            ChartDocumentError{"chart document does not exist"});
    auto window = ChartWindow(workspace, document_id);
    if (!window) return std::unexpected(window.error());
    (*window)->open = false;
    return {};
}

std::expected<void, ChartDocumentError> ReopenChartDocument(
    WorkspaceState& workspace, std::string_view document_id) {
    if (FindChartDocument(workspace, document_id) == nullptr)
        return std::unexpected(
            ChartDocumentError{"chart document does not exist"});
    auto window = ChartWindow(workspace, document_id);
    if (!window) return std::unexpected(window.error());
    (*window)->open = true;
    return {};
}

std::expected<void, ChartDocumentError> DeleteChartDocument(
    WorkspaceState& workspace, std::string_view document_id) {
    const auto chart = std::ranges::find(
        workspace.charts, document_id, &ChartDocumentState::id);
    if (chart == workspace.charts.end())
        return std::unexpected(
            ChartDocumentError{"chart document does not exist"});
    workspace.charts.erase(chart);
    workspace.windows.erase(document_id);
    return {};
}

std::expected<std::string, ChartDocumentError>
SaveIndicatorSuiteFromChart(WorkspaceState& workspace,
                            std::string_view document_id,
                            std::string name) {
    const ChartDocumentState* chart =
        FindChartDocument(workspace, document_id);
    if (chart == nullptr)
        return std::unexpected(
            ChartDocumentError{"chart document does not exist"});
    if (name.empty())
        return std::unexpected(
            ChartDocumentError{"indicator suite name is required"});
    if (std::ranges::any_of(
            workspace.indicator_suites,
            [&name](const IndicatorSuiteState& suite) {
                return suite.name == name;
            }))
        return std::unexpected(
            ChartDocumentError{"indicator suite name already exists"});

    const std::string id = NewStableId("indicator-suite");
    workspace.indicator_suites.push_back({
        .id = id,
        .name = std::move(name),
        .indicators = CloneIndicators(chart->indicators),
    });
    return id;
}

std::expected<void, ChartDocumentError> ApplyIndicatorSuite(
    WorkspaceState& workspace, std::string_view document_id,
    std::string_view suite_id) {
    ChartDocumentState* chart = FindChartDocument(workspace, document_id);
    if (chart == nullptr)
        return std::unexpected(
            ChartDocumentError{"chart document does not exist"});
    const auto suite = std::ranges::find(
        workspace.indicator_suites, suite_id, &IndicatorSuiteState::id);
    if (suite == workspace.indicator_suites.end())
        return std::unexpected(
            ChartDocumentError{"indicator suite does not exist"});
    chart->indicators = CloneIndicators(suite->indicators);
    return {};
}

std::expected<void, ChartDocumentError> SetChartDefaultsFromDocument(
    WorkspaceState& workspace, std::string_view document_id) {
    const ChartDocumentState* chart =
        FindChartDocument(workspace, document_id);
    if (chart == nullptr)
        return std::unexpected(
            ChartDocumentError{"chart document does not exist"});
    workspace.chart_defaults = {
        .timeframe = chart->timeframe,
        .feed = chart->feed,
        .adjustment = chart->adjustment,
        .visible_bars = chart->visible_bars,
        .show_volume = chart->show_volume,
        .show_close_line = chart->show_close_line,
        .show_crosshair = chart->show_crosshair,
        .indicators = CloneIndicators(chart->indicators),
    };
    return {};
}

std::expected<std::string, ChartDocumentError> AddChartIndicator(
    WorkspaceState& workspace, std::string_view document_id,
    ChartIndicatorState indicator) {
    ChartDocumentState* chart = FindChartDocument(workspace, document_id);
    if (chart == nullptr)
        return std::unexpected(
            ChartDocumentError{"chart document does not exist"});
    const core::IndicatorInput& input = std::visit(
        [](const auto& calculation) -> const core::IndicatorInput& {
            return calculation.input;
        },
        indicator.definition.calculation);
    if (const auto* reference =
            std::get_if<core::IndicatorOutputInput>(&input);
        reference != nullptr &&
        std::ranges::none_of(
            chart->indicators,
            [&](const ChartIndicatorState& existing) {
                return existing.definition.id == reference->indicator_id;
            }))
        return std::unexpected(
            ChartDocumentError{"indicator input does not exist in chart"});
    indicator.definition.id = NewStableId("indicator");
    const std::string id = indicator.definition.id;
    chart->indicators.push_back(std::move(indicator));
    return id;
}

bool RemoveChartIndicator(WorkspaceState& workspace,
                          std::string_view document_id,
                          std::string_view indicator_id) {
    ChartDocumentState* chart = FindChartDocument(workspace, document_id);
    if (chart == nullptr) return false;
    if (std::ranges::any_of(
            chart->indicators,
            [&](const ChartIndicatorState& indicator) {
                const auto* reference =
                    std::visit(
                        [](const auto& calculation) {
                            return std::get_if<
                                core::IndicatorOutputInput>(
                                &calculation.input);
                        },
                        indicator.definition.calculation);
                return reference != nullptr &&
                       reference->indicator_id == indicator_id;
            }))
        return false;
    const auto found = std::ranges::find(
        chart->indicators, indicator_id,
        [](const ChartIndicatorState& indicator) {
            return indicator.definition.id;
        });
    if (found == chart->indicators.end()) return false;
    chart->indicators.erase(found);
    return true;
}

std::expected<std::string, ChartDocumentError> CreateChartDrawing(
    WorkspaceState& workspace, ChartDrawingState drawing) {
    if (!drawing.id.empty())
        return std::unexpected(
            ChartDocumentError{"new drawing must not provide an identity"});
    drawing.id = NewStableId("drawing");
    const std::string id = drawing.id;
    auto inserted = UpsertChartDrawing(workspace, std::move(drawing));
    if (!inserted) return std::unexpected(inserted.error());
    return id;
}

std::expected<void, ChartDocumentError> UpsertChartDrawing(
    WorkspaceState& workspace, ChartDrawingState drawing) {
    if (drawing.id.empty() || drawing.instrument_id.empty())
        return std::unexpected(ChartDocumentError{
            "drawing requires stable drawing and instrument identities"});
    const auto found = std::ranges::find(
        workspace.chart_drawings, drawing.id, &ChartDrawingState::id);
    if (found != workspace.chart_drawings.end()) {
        if (found->instrument_id != drawing.instrument_id)
            return std::unexpected(ChartDocumentError{
                "drawing identity cannot move between instruments"});
        *found = std::move(drawing);
    } else {
        workspace.chart_drawings.push_back(std::move(drawing));
    }
    return {};
}

bool DeleteChartDrawing(WorkspaceState& workspace,
                        std::string_view drawing_id) {
    const auto found = std::ranges::find(
        workspace.chart_drawings, drawing_id, &ChartDrawingState::id);
    if (found == workspace.chart_drawings.end()) return false;
    workspace.chart_drawings.erase(found);
    return true;
}

std::vector<std::reference_wrapper<const ChartDrawingState>>
DrawingsForInstrument(const WorkspaceState& workspace,
                      std::string_view instrument_id) {
    std::vector<std::reference_wrapper<const ChartDrawingState>> result;
    for (const ChartDrawingState& drawing : workspace.chart_drawings)
        if (drawing.instrument_id == instrument_id)
            result.emplace_back(drawing);
    return result;
}

}  // namespace tradebox::workstation
