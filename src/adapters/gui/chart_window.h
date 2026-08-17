#pragma once

#include "tradebox/application/trading_application.h"
#include "tradebox/ui/workspace.h"
#include "tradebox/workstation/chart_commands.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tradebox::gui {

class ChartWindowRenderer final {
public:
    explicit ChartWindowRenderer(
        workstation::ChartEditHistory& edit_history)
        : edit_history_(edit_history) {}

    [[nodiscard]] application::UiSnapshotQuery BuildSnapshotQuery(
        workstation::WorkspaceState& state, std::int64_t now_ns);
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              const application::ApplicationUiSnapshot& snapshot);

    [[nodiscard]] bool ConsumePersistentChanges();

private:
    struct InteractionState {
        std::array<char, 64> ticker{};
        bool initialized = false;
        bool resolve_requested = false;
        std::int64_t effective_anchor_ns = 0;
        bool panning = false;
        float pan_origin_x = 0.0F;
        std::int64_t pan_origin_anchor_ns = 0;
        std::int64_t preview_anchor_ns = 0;
        std::optional<workstation::ChartDrawingKind> drawing_tool;
        std::optional<workstation::ChartDrawingAnchorState> drawing_first;
        std::string selected_drawing_id;
        std::optional<workstation::ChartDrawingState> drawing_preview;
        std::int64_t drawing_drag_start_time_ns = 0;
        core::Decimal drawing_drag_start_price;
    };

    [[nodiscard]] InteractionState& Interaction(
        const workstation::ChartDocumentState& chart);
    [[nodiscard]] const application::UiChartSnapshot* SnapshotFor(
        std::string_view document_id,
        const application::ApplicationUiSnapshot& snapshot) const;
    void DrawChartWindow(ui::Workspace& workspace,
                         workstation::WorkspaceState& state,
                         workstation::ChartDocumentState& chart,
                         const application::UiChartSnapshot* snapshot,
                         const application::ApplicationUiSnapshot& app_snapshot);
    void DrawSeries(const application::UiChartSnapshot& snapshot,
                    workstation::WorkspaceState& state,
                    workstation::ChartDocumentState& chart_state,
                    InteractionState& interaction);

    workstation::ChartEditHistory& edit_history_;
    std::unordered_map<std::string, InteractionState> interactions_;
    bool persistent_changed_ = false;
};

}  // namespace tradebox::gui
