#include "tradebox/workstation/order_tickets.h"

#include "tradebox/workstation/stable_id.h"

#include <algorithm>

namespace tradebox::workstation {
namespace {

std::expected<void, std::string> EnsureOrderTicketWindow(
    WorkspaceState& state) {
    auto [found, inserted] = state.windows.try_emplace(
        std::string(kOrderTicketWindowId), WindowInstanceState{
            .id = std::string(kOrderTicketWindowId),
            .kind = "order-ticket",
            .title = "Order Ticket",
            .open = true,
            .bounds = {24.0f, 72.0f, 360.0f, 420.0f},
        });
    if (!inserted && found->second.kind != "order-ticket")
        return std::unexpected("Order ticket window ID is owned by another window");
    found->second.open = true;
    return {};
}

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

OrderTicketState* FindOrderTicketForSymbol(WorkspaceState& state,
                                           std::string_view symbol) {
    const auto found = std::ranges::find(
        state.order_tickets, symbol, &OrderTicketState::symbol);
    return found == state.order_tickets.end() ? nullptr : &*found;
}

const OrderTicketState* FindOrderTicketForSymbol(const WorkspaceState& state,
                                                 std::string_view symbol) {
    const auto found = std::ranges::find(
        state.order_tickets, symbol, &OrderTicketState::symbol);
    return found == state.order_tickets.end() ? nullptr : &*found;
}

std::expected<void, std::string> OpenOrderTicketForSymbol(
    WorkspaceState& state, std::string_view symbol) {
    if (symbol.empty()) return std::unexpected("Order ticket symbol cannot be empty");
    if (const auto ensured = EnsureOrderTicketWindow(state); !ensured)
        return ensured;

    state.selected_symbol = std::string(symbol);
    if (FindOrderTicketForSymbol(state, symbol) == nullptr) {
        state.order_tickets.push_back({
            .id = NewStableId("order-ticket"),
            .symbol = std::string(symbol),
        });
    }
    return {};
}

std::expected<void, std::string> OpenTradeHotkeyWindow(WorkspaceState& state) {
    return EnsureTradeHotkeyWindow(state);
}

}  // namespace tradebox::workstation
