#pragma once

#include "tradebox/workstation/state.h"

#include <expected>
#include <string>
#include <string_view>

namespace tradebox::workstation {

inline constexpr std::string_view kTradeHotkeyWindowId =
    "trade-hotkey.window";

[[nodiscard]] std::expected<void, std::string> OpenTradeHotkeyWindow(
    WorkspaceState& state);

}  // namespace tradebox::workstation
