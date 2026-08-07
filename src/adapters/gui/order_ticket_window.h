#pragma once

#include "tradebox/application/ui_snapshot.h"
#include "tradebox/ui/workspace.h"

#include <array>
#include <string>
#include <unordered_map>

namespace tradebox::gui {

class OrderTicketWindowRenderer final {
public:
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              const application::ApplicationUiSnapshot& snapshot);
    [[nodiscard]] bool ConsumePersistentChanges();

private:
    // ImGui edits text through a stable char buffer, not a std::string. These
    // buffers are ephemeral per-ticket edit state: they mirror the persistent
    // OrderTicketState strings and are written back only when the user edits
    // (same pattern as chart_window's InteractionState).
    struct InteractionState {
        std::array<char, 96> amount{};
        std::array<char, 96> limit_price{};
        std::array<char, 96> stop_price{};
        bool initialized = false;
    };

    [[nodiscard]] InteractionState& Interaction(
        const workstation::OrderTicketState& ticket);

    std::unordered_map<std::string, InteractionState> interactions_;
    bool persistent_changed_ = false;
};

}  // namespace tradebox::gui