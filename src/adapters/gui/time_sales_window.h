#pragma once

#include "tradebox/application/ui_snapshot.h"
#include "tradebox/core/market_data.h"
#include "tradebox/ui/workspace.h"

#include <string>

namespace tradebox::gui {

class TimeSalesWindowRenderer final {
public:
    void AppendSnapshotQuery(workstation::WorkspaceState& state,
                             application::UiSnapshotQuery& query);
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              const application::ApplicationUiSnapshot& snapshot);
    [[nodiscard]] bool ConsumePersistentChanges();

private:
    bool persistent_changed_ = false;
};

}  // namespace tradebox::gui
