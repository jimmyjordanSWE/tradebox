#pragma once

#include "tradebox/ui/workspace.h"

namespace tradebox::gui {

class TradeHotkeyWindowRenderer final {
public:
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state);
    [[nodiscard]] bool ConsumePersistentChanges();

private:
    bool persistent_changed_ = false;
};

}  // namespace tradebox::gui
