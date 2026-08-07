#pragma once

#include "tradebox/workstation/state.h"

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace tradebox::workstation {

inline constexpr std::string_view kPositionsWindowId = "positions.window";
inline constexpr std::string_view kOrdersWindowId = "orders.window";
inline constexpr std::string_view kPositionsTableId = "positions";
inline constexpr std::string_view kOrdersTableId = "orders";

struct TableColumnDefinition {
    std::string_view id;
    std::string_view label;
    float default_width = 125.0f;
    bool required = false;
};

[[nodiscard]] std::span<const TableColumnDefinition>
PositionColumnDefinitions();
[[nodiscard]] std::span<const TableColumnDefinition>
OrderColumnDefinitions();
[[nodiscard]] const TableColumnDefinition* FindPositionColumn(
    std::string_view id);
[[nodiscard]] const TableColumnDefinition* FindOrderColumn(
    std::string_view id);

[[nodiscard]] std::expected<void, std::string> CreatePositionsWindow(
    WorkspaceState& state);
[[nodiscard]] std::expected<void, std::string> CreateOrdersWindow(
    WorkspaceState& state);
[[nodiscard]] std::expected<void, std::string> AddPositionColumn(
    WorkspaceState& state, std::string_view id);
[[nodiscard]] std::expected<void, std::string> RemovePositionColumn(
    WorkspaceState& state, std::string_view id);
[[nodiscard]] std::expected<void, std::string> AddOrderColumn(
    WorkspaceState& state, std::string_view id);
[[nodiscard]] std::expected<void, std::string> RemoveOrderColumn(
    WorkspaceState& state, std::string_view id);

}  // namespace tradebox::workstation
