#pragma once

#include "tradebox/application/trading_application.h"
#include "tradebox/ui/workspace.h"

#include <array>
#include <string>
#include <unordered_map>

namespace tradebox::gui {

class WatchListWindowRenderer final {
public:
    void AppendSnapshotQuery(
        workstation::WorkspaceState& state,
        application::UiSnapshotQuery& query) const;
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              const application::ApplicationUiSnapshot& snapshot);
    [[nodiscard]] bool ConsumePersistentChanges();

private:
    struct RowInteraction {
        std::array<char, 64> ticker{};
        bool initialized = false;
    };

    std::unordered_map<std::string, RowInteraction> interactions_;
    bool persistent_changed_ = false;
};

}  // namespace tradebox::gui
