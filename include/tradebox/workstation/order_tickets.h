#pragma once

#include "tradebox/workstation/state.h"

#include <expected>
#include <string>
#include <string_view>

namespace tradebox::workstation {

inline constexpr std::string_view kOrderTicketWindowId =
    "order-ticket.window";
inline constexpr std::string_view kTradeHotkeyWindowId =
    "trade-hotkey.window";

// Selects the single ticket configuration for a ticker and makes the existing
// persistent Order Ticket window visible. Ticket settings belong to the
// workstation profile, not to a watch-list row or GUI interaction state.
[[nodiscard]] std::expected<void, std::string> OpenOrderTicketForSymbol(
    WorkspaceState& state, std::string_view symbol);

[[nodiscard]] std::expected<void, std::string> OpenTradeHotkeyWindow(
    WorkspaceState& state);

[[nodiscard]] OrderTicketState* FindOrderTicketForSymbol(
    WorkspaceState& state, std::string_view symbol);
[[nodiscard]] const OrderTicketState* FindOrderTicketForSymbol(
    const WorkspaceState& state, std::string_view symbol);

}  // namespace tradebox::workstation
