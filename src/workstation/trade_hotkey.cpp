#include "tradebox/workstation/trade_hotkey.h"

namespace tradebox::workstation {
namespace {

std::expected<void, std::string> EnsureTradeHotkeyWindow(
    WorkspaceState& state) {
    auto [found, inserted] = state.windows.try_emplace(
        std::string(kTradeHotkeyWindowId), WindowInstanceState{
            .id = std::string(kTradeHotkeyWindowId),
            .kind = "trade-hotkey",
            .title = "Trade Hotkey",
            .open = true,
            .bounds = {820.0f, 72.0f, 300.0f, 230.0f},
        });
    if (!inserted && found->second.kind != "trade-hotkey")
        return std::unexpected("Trade hotkey window ID is owned by another window");
    found->second.open = true;
    return {};
}

}  // namespace

std::expected<void, std::string> OpenTradeHotkeyWindow(WorkspaceState& state) {
    return EnsureTradeHotkeyWindow(state);
}

}  // namespace tradebox::workstation
