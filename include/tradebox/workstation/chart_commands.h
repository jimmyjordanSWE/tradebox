#pragma once

#include "tradebox/workstation/state.h"

#include <cstddef>
#include <expected>
#include <string>
#include <variant>
#include <vector>

namespace tradebox::workstation {

inline constexpr std::size_t kMaximumChartEditHistory = 256;

struct SetChartViewportCommand {
    std::string document_id;
    int visible_bars = 120;
    std::int64_t range_anchor_ns = 0;
};

struct AddChartIndicatorCommand {
    std::string document_id;
    ChartIndicatorState indicator;
};

struct UpdateChartIndicatorCommand {
    std::string document_id;
    ChartIndicatorState indicator;
};

struct RemoveChartIndicatorCommand {
    std::string document_id;
    std::string indicator_id;
};

struct AddChartDrawingCommand {
    ChartDrawingState drawing;
};

struct UpdateChartDrawingCommand {
    ChartDrawingState drawing;
};

struct RemoveChartDrawingCommand {
    std::string drawing_id;
};

using ChartEditCommand = std::variant<
    SetChartViewportCommand,
    AddChartIndicatorCommand,
    UpdateChartIndicatorCommand,
    RemoveChartIndicatorCommand,
    AddChartDrawingCommand,
    UpdateChartDrawingCommand,
    RemoveChartDrawingCommand>;

struct ChartEditError {
    std::string message;
};

struct AppliedChartEdit {
    ChartEditCommand inverse;
    std::vector<std::string> affected_document_ids;
};

[[nodiscard]] std::expected<AppliedChartEdit, ChartEditError>
ApplyChartEdit(WorkspaceState& state, const ChartEditCommand& command);

class ChartEditHistory final {
public:
    [[nodiscard]] std::expected<AppliedChartEdit, ChartEditError> Execute(
        WorkspaceState& state, const ChartEditCommand& command);
    [[nodiscard]] std::expected<AppliedChartEdit, ChartEditError> Undo(
        WorkspaceState& state);
    [[nodiscard]] std::expected<AppliedChartEdit, ChartEditError> Redo(
        WorkspaceState& state);

    [[nodiscard]] bool CanUndo() const { return !undo_.empty(); }
    [[nodiscard]] bool CanRedo() const { return !redo_.empty(); }
    [[nodiscard]] std::size_t UndoSize() const { return undo_.size(); }
    [[nodiscard]] std::size_t RedoSize() const { return redo_.size(); }
    void Clear();

private:
    static void PushBounded(std::vector<ChartEditCommand>& destination,
                            ChartEditCommand command);

    std::vector<ChartEditCommand> undo_;
    std::vector<ChartEditCommand> redo_;
};

}  // namespace tradebox::workstation
