#include "tradebox/workstation/event_window.h"
namespace tradebox::workstation {
std::expected<void, std::string> CreateEventWindow(WorkspaceState& state) {
    auto [it, inserted] = state.windows.emplace(std::string(kEventWindowId), WindowInstanceState{.id=std::string(kEventWindowId), .kind="events", .title="Events", .open=true});
    if (!inserted) it->second.open = true;
    return {};
}
}
