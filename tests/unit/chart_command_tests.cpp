#include "tradebox/workstation/chart_commands.h"
#include "tradebox/workstation/chart_documents.h"

#include <gtest/gtest.h>

namespace tradebox::workstation {
namespace {

WorkspaceState ChartWorkspace() {
    WorkspaceState state;
    const auto created = CreateChartDocument(
        state, {.instrument_id = "asset-aapl", .symbol = "AAPL"});
    EXPECT_TRUE(created);
    return state;
}

ChartDrawingState Horizontal(std::string id) {
    return {
        .id = std::move(id),
        .instrument_id = "asset-aapl",
        .kind = ChartDrawingKind::HorizontalLine,
        .first = {.time_ns = 10,
                  .price = *core::Decimal::Parse("100")},
    };
}

TEST(ChartCommands, ViewportIsValidatedAndReversible) {
    WorkspaceState state = ChartWorkspace();
    const std::string id = state.charts.front().id;
    const auto applied = ApplyChartEdit(
        state, SetChartViewportCommand{id, 240, 1'000});
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(state.charts.front().visible_bars, 240);
    EXPECT_EQ(state.charts.front().range_anchor_ns, 1'000);

    ASSERT_TRUE(ApplyChartEdit(state, applied->inverse));
    EXPECT_EQ(state.charts.front().visible_bars, 120);
    EXPECT_EQ(state.charts.front().range_anchor_ns, 0);

    const WorkspaceState unchanged = state;
    EXPECT_FALSE(ApplyChartEdit(
        state, SetChartViewportCommand{id, 1, 1'000}));
    EXPECT_EQ(state.charts.front().visible_bars,
              unchanged.charts.front().visible_bars);
}

TEST(ChartCommands, DrawingCommandsPreserveStableIdentity) {
    WorkspaceState state = ChartWorkspace();
    ChartEditHistory history;
    ASSERT_TRUE(history.Execute(
        state, AddChartDrawingCommand{Horizontal("drawing-1")}));
    ASSERT_EQ(state.chart_drawings.size(), 1U);
    EXPECT_EQ(state.chart_drawings.front().id, "drawing-1");

    ASSERT_TRUE(history.Undo(state));
    EXPECT_TRUE(state.chart_drawings.empty());
    ASSERT_TRUE(history.Redo(state));
    ASSERT_EQ(state.chart_drawings.size(), 1U);
    EXPECT_EQ(state.chart_drawings.front().id, "drawing-1");
}

TEST(ChartCommands, FailedHistoryOperationDoesNotMoveStacks) {
    WorkspaceState state = ChartWorkspace();
    ChartEditHistory history;
    EXPECT_FALSE(history.Execute(
        state, RemoveChartDrawingCommand{"missing"}));
    EXPECT_FALSE(history.CanUndo());
    EXPECT_FALSE(history.CanRedo());
}

TEST(ChartCommands, HistoryIsBoundedAndNewEditClearsRedo) {
    WorkspaceState state = ChartWorkspace();
    ChartEditHistory history;
    const std::string id = state.charts.front().id;
    for (std::size_t index = 0; index < kMaximumChartEditHistory + 5;
         ++index) {
        ASSERT_TRUE(history.Execute(
            state, SetChartViewportCommand{
                       id, 120, static_cast<std::int64_t>(index + 1)}));
    }
    EXPECT_EQ(history.UndoSize(), kMaximumChartEditHistory);
    ASSERT_TRUE(history.Undo(state));
    EXPECT_TRUE(history.CanRedo());
    ASSERT_TRUE(history.Execute(
        state, SetChartViewportCommand{id, 120, 2'000}));
    EXPECT_FALSE(history.CanRedo());
}

}  // namespace
}  // namespace tradebox::workstation
