#pragma once

#include "tradebox/application/ui_snapshot.h"
#include "tradebox/ui/workspace.h"

namespace tradebox::gui {

class PositionsWindowRenderer final {
public:
    void AppendSnapshotQuery(workstation::WorkspaceState& state,
                             application::UiSnapshotQuery& query);
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              const application::ApplicationUiSnapshot& snapshot);
    [[nodiscard]] bool ConsumePersistentChanges();

private:
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
