#pragma once

#include "tradebox/application/trading_application.h"
#include "tradebox/ui/workspace.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tradebox::gui {

class ChartWindowRenderer final {
public:
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

    std::unordered_map<std::string, InteractionState> interactions_;
    bool persistent_changed_ = false;
};

}  // namespace tradebox::gui
