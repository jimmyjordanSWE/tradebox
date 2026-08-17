#include "tradebox/workstation/chart_commands.h"

#include "tradebox/workstation/chart_documents.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace tradebox::workstation {
namespace {

constexpr int kMinimumVisibleBars = 30;
constexpr int kMaximumVisibleBars = 2'000;

ChartEditError Error(std::string message) {
    return ChartEditError{std::move(message)};
}

std::vector<std::string> DrawingDocuments(
    const WorkspaceState& state, std::string_view instrument_id) {
    std::vector<std::string> result;
    for (const ChartDocumentState& chart : state.charts)
        if (chart.instrument_id == instrument_id)
            result.push_back(chart.id);
    return result;
}

bool DrawingAnchorsValid(const ChartDrawingState& drawing) {
    const bool needs_second =
        drawing.kind == ChartDrawingKind::TrendLine ||
        drawing.kind == ChartDrawingKind::Ray ||
        drawing.kind == ChartDrawingKind::Rectangle;
    return needs_second == drawing.second.has_value();
}

std::expected<void, ChartEditError> ValidateDrawing(
    const ChartDrawingState& drawing) {
    if (drawing.id.empty() || drawing.instrument_id.empty())
        return std::unexpected(
            Error("drawing requires stable drawing and instrument identities"));
    if (!DrawingAnchorsValid(drawing))
        return std::unexpected(Error("drawing anchor count does not match kind"));
    if (!std::isfinite(drawing.line_width) || drawing.line_width < 0.5F ||
        drawing.line_width > 10.0F)
        return std::unexpected(Error("drawing line width is invalid"));
    return {};
}

std::expected<void, ChartEditError> ValidateIndicators(
    const std::vector<ChartIndicatorState>& indicators) {
    std::vector<core::IndicatorDefinition> definitions;
    definitions.reserve(indicators.size());
    for (const ChartIndicatorState& indicator : indicators) {
        if (indicator.definition.id.empty())
            return std::unexpected(Error("indicator identity is required"));
        if (!std::isfinite(indicator.line_width) ||
            indicator.line_width < 0.5F || indicator.line_width > 10.0F)
            return std::unexpected(Error("indicator line width is invalid"));
        definitions.push_back(indicator.definition);
    }
    const auto evaluated = core::EvaluateIndicators(definitions, {});
    if (!evaluated)
        return std::unexpected(Error(evaluated.error().message));
    return {};
}

std::expected<AppliedChartEdit, ChartEditError> Apply(
    WorkspaceState& state, const SetChartViewportCommand& command) {
    ChartDocumentState* chart = FindChartDocument(state, command.document_id);
    if (chart == nullptr)
        return std::unexpected(Error("chart document does not exist"));
    if (command.visible_bars < kMinimumVisibleBars ||
        command.visible_bars > kMaximumVisibleBars ||
        command.range_anchor_ns < 0)
        return std::unexpected(Error("chart viewport is outside supported limits"));
    const SetChartViewportCommand inverse{
        .document_id = chart->id,
        .visible_bars = chart->visible_bars,
        .range_anchor_ns = chart->range_anchor_ns,
    };
    chart->visible_bars = command.visible_bars;
    chart->range_anchor_ns = command.range_anchor_ns;
    return AppliedChartEdit{.inverse = inverse,
                            .affected_document_ids = {chart->id}};
}

std::expected<AppliedChartEdit, ChartEditError> Apply(
    WorkspaceState& state, const AddChartIndicatorCommand& command) {
    ChartDocumentState* chart = FindChartDocument(state, command.document_id);
    if (chart == nullptr)
        return std::unexpected(Error("chart document does not exist"));
    if (std::ranges::any_of(
            chart->indicators, [&](const ChartIndicatorState& value) {
                return value.definition.id == command.indicator.definition.id;
            }))
        return std::unexpected(Error("indicator identity already exists"));
    auto candidate = chart->indicators;
    candidate.push_back(command.indicator);
    if (auto valid = ValidateIndicators(candidate); !valid)
        return std::unexpected(valid.error());
    chart->indicators = std::move(candidate);
    return AppliedChartEdit{
        .inverse = RemoveChartIndicatorCommand{
            .document_id = chart->id,
            .indicator_id = command.indicator.definition.id},
        .affected_document_ids = {chart->id}};
}

std::expected<AppliedChartEdit, ChartEditError> Apply(
    WorkspaceState& state, const UpdateChartIndicatorCommand& command) {
    ChartDocumentState* chart = FindChartDocument(state, command.document_id);
    if (chart == nullptr)
        return std::unexpected(Error("chart document does not exist"));
    const auto found = std::ranges::find(
        chart->indicators, command.indicator.definition.id,
        [](const ChartIndicatorState& value) { return value.definition.id; });
    if (found == chart->indicators.end())
        return std::unexpected(Error("indicator does not exist"));
    auto candidate = chart->indicators;
    const auto candidate_found = std::ranges::find(
        candidate, command.indicator.definition.id,
        [](const ChartIndicatorState& value) { return value.definition.id; });
    const ChartIndicatorState previous = *candidate_found;
    *candidate_found = command.indicator;
    if (auto valid = ValidateIndicators(candidate); !valid)
        return std::unexpected(valid.error());
    chart->indicators = std::move(candidate);
    return AppliedChartEdit{
        .inverse = UpdateChartIndicatorCommand{.document_id = chart->id,
                                                .indicator = previous},
        .affected_document_ids = {chart->id}};
}

std::expected<AppliedChartEdit, ChartEditError> Apply(
    WorkspaceState& state, const RemoveChartIndicatorCommand& command) {
    ChartDocumentState* chart = FindChartDocument(state, command.document_id);
    if (chart == nullptr)
        return std::unexpected(Error("chart document does not exist"));
    const auto found = std::ranges::find(
        chart->indicators, command.indicator_id,
        [](const ChartIndicatorState& value) { return value.definition.id; });
    if (found == chart->indicators.end())
        return std::unexpected(Error("indicator does not exist"));
    if (std::ranges::any_of(
            chart->indicators,
            [&](const ChartIndicatorState& indicator) {
                return std::visit(
                    [&](const auto& calculation) {
                        const auto* input =
                            std::get_if<core::IndicatorOutputInput>(
                                &calculation.input);
                        return input != nullptr &&
                               input->indicator_id == command.indicator_id;
                    },
                    indicator.definition.calculation);
            }))
        return std::unexpected(Error("indicator is used by another indicator"));
    const ChartIndicatorState removed = *found;
    chart->indicators.erase(found);
    return AppliedChartEdit{
        .inverse = AddChartIndicatorCommand{.document_id = chart->id,
                                             .indicator = removed},
        .affected_document_ids = {chart->id}};
}

std::expected<AppliedChartEdit, ChartEditError> Apply(
    WorkspaceState& state, const AddChartDrawingCommand& command) {
    if (auto valid = ValidateDrawing(command.drawing); !valid)
        return std::unexpected(valid.error());
    if (std::ranges::any_of(
            state.chart_drawings, [&](const ChartDrawingState& value) {
                return value.id == command.drawing.id;
            }))
        return std::unexpected(Error("drawing identity already exists"));
    state.chart_drawings.push_back(command.drawing);
    return AppliedChartEdit{
        .inverse = RemoveChartDrawingCommand{command.drawing.id},
        .affected_document_ids =
            DrawingDocuments(state, command.drawing.instrument_id)};
}

std::expected<AppliedChartEdit, ChartEditError> Apply(
    WorkspaceState& state, const UpdateChartDrawingCommand& command) {
    if (auto valid = ValidateDrawing(command.drawing); !valid)
        return std::unexpected(valid.error());
    const auto found = std::ranges::find(
        state.chart_drawings, command.drawing.id, &ChartDrawingState::id);
    if (found == state.chart_drawings.end())
        return std::unexpected(Error("drawing does not exist"));
    if (found->instrument_id != command.drawing.instrument_id)
        return std::unexpected(
            Error("drawing identity cannot move between instruments"));
    const ChartDrawingState previous = *found;
    *found = command.drawing;
    return AppliedChartEdit{
        .inverse = UpdateChartDrawingCommand{previous},
        .affected_document_ids =
            DrawingDocuments(state, command.drawing.instrument_id)};
}

std::expected<AppliedChartEdit, ChartEditError> Apply(
    WorkspaceState& state, const RemoveChartDrawingCommand& command) {
    const auto found = std::ranges::find(
        state.chart_drawings, command.drawing_id, &ChartDrawingState::id);
    if (found == state.chart_drawings.end())
        return std::unexpected(Error("drawing does not exist"));
    const ChartDrawingState removed = *found;
    state.chart_drawings.erase(found);
    return AppliedChartEdit{
        .inverse = AddChartDrawingCommand{removed},
        .affected_document_ids =
            DrawingDocuments(state, removed.instrument_id)};
}

}  // namespace

std::expected<AppliedChartEdit, ChartEditError> ApplyChartEdit(
    WorkspaceState& state, const ChartEditCommand& command) {
    return std::visit(
        [&](const auto& value) { return Apply(state, value); }, command);
}

void ChartEditHistory::PushBounded(
    std::vector<ChartEditCommand>& destination, ChartEditCommand command) {
    if (destination.size() == kMaximumChartEditHistory)
        destination.erase(destination.begin());
    destination.push_back(std::move(command));
}

std::expected<AppliedChartEdit, ChartEditError> ChartEditHistory::Execute(
    WorkspaceState& state, const ChartEditCommand& command) {
    auto applied = ApplyChartEdit(state, command);
    if (!applied) return std::unexpected(applied.error());
    PushBounded(undo_, applied->inverse);
    redo_.clear();
    return applied;
}

std::expected<AppliedChartEdit, ChartEditError> ChartEditHistory::Undo(
    WorkspaceState& state) {
    if (undo_.empty()) return std::unexpected(Error("nothing to undo"));
    auto applied = ApplyChartEdit(state, undo_.back());
    if (!applied) return std::unexpected(applied.error());
    undo_.pop_back();
    PushBounded(redo_, applied->inverse);
    return applied;
}

std::expected<AppliedChartEdit, ChartEditError> ChartEditHistory::Redo(
    WorkspaceState& state) {
    if (redo_.empty()) return std::unexpected(Error("nothing to redo"));
    auto applied = ApplyChartEdit(state, redo_.back());
    if (!applied) return std::unexpected(applied.error());
    redo_.pop_back();
    PushBounded(undo_, applied->inverse);
    return applied;
}

void ChartEditHistory::Clear() {
    undo_.clear();
    redo_.clear();
}

}  // namespace tradebox::workstation
