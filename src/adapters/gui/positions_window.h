#pragma once

#include "tradebox/application/ui_snapshot.h"
#include "tradebox/ui/workspace.h"

#include <string>
#include <optional>

namespace tradebox::gui {

class PositionsWindowRenderer final {
public:
    void AppendSnapshotQuery(workstation::WorkspaceState& state,
                             application::UiSnapshotQuery& query);
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              const application::ApplicationUiSnapshot& snapshot);
    [[nodiscard]] std::optional<std::string> ConsumeExitRequest();
    [[nodiscard]] bool ConsumePersistentChanges();

private:
    std::optional<std::string> exit_confirmation_symbol_;
    std::optional<std::string> exit_request_;
    bool persistent_changed_ = false;
};

class OrdersWindowRenderer final {
public:
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              const application::ApplicationUiSnapshot& snapshot);
    [[nodiscard]] bool ConsumePersistentChanges();

private:
    bool persistent_changed_ = false;
};

}  // namespace tradebox::gui
