#include "tradebox/workstation/positions_orders_windows.h"

#include <array>
#include <algorithm>

namespace tradebox::workstation {
namespace {

constexpr auto kPositionColumns = std::to_array<TableColumnDefinition>({
    {"symbol", "Symbol", 110.0f, true},
    {"qty", "Qty", 90.0f}, {"qty_available", "Available", 100.0f},
    {"side", "Side", 80.0f}, {"avg_entry", "Avg Entry", 105.0f},
    {"last", "Last", 100.0f}, {"prev_close", "Prev Close", 105.0f},
    {"market_value", "Market Value", 125.0f},
    {"cost_basis", "Cost Basis", 115.0f}, {"pnl", "Unrealized P&L", 125.0f},
    {"pnl_percent", "Unrealized P&L %", 135.0f},
    {"day_pnl", "Day P&L", 105.0f}, {"day_pnl_percent", "Day P&L %", 115.0f},
    {"change_today", "Day Change %", 120.0f}, {"exchange", "Exchange", 95.0f},
    {"asset_class", "Asset Class", 105.0f}, {"valuation", "Valuation", 120.0f},
    {"asset_id", "Asset ID", 180.0f}, {"provisional", "Provisional", 95.0f},
    {"valuation_current", "Valuation Current", 130.0f},
    {"valuation_stream", "Valuation Stream", 130.0f},
    {"valuation_feed", "Valuation Feed", 115.0f},
    {"valuation_event_ns", "Valuation Event (ns)", 165.0f},
    {"valuation_received_ms", "Valuation Received (ms)", 180.0f},
});

constexpr auto kOrderColumns = std::to_array<TableColumnDefinition>({
    {"symbol", "Symbol", 110.0f, true},
    {"side", "Side", 75.0f}, {"status", "Status", 105.0f},
    {"type", "Type", 95.0f}, {"qty", "Shares", 85.0f},
    {"notional", "Order Notional", 110.0f}, {"filled", "Filled Shares", 100.0f},
    {"filled_avg", "Average Fill", 105.0f}, {"filled_value", "Filled Value", 110.0f}, {"limit", "Limit", 90.0f},
    {"stop", "Stop", 90.0f}, {"tif", "TIF", 70.0f},
    {"class", "Class", 90.0f}, {"extended_hours", "Extended", 90.0f},
    {"submitted", "Submitted", 170.0f}, {"updated", "Updated", 170.0f},
    {"client_order_id", "Client Order ID", 180.0f},
    {"order_id", "Order ID", 180.0f}, {"parent_order_id", "Parent Order ID", 180.0f},
    {"asset_id", "Asset ID", 180.0f}, {"asset_class", "Asset Class", 105.0f},
    {"filled_at", "Filled", 170.0f}, {"canceled_at", "Cancelled", 170.0f},
    {"expired_at", "Expired", 170.0f}, {"failed_at", "Failed", 170.0f},
    {"replaced_at", "Replaced", 170.0f}, {"replaced_by", "Replaced By", 180.0f},
    {"replaces", "Replaces", 180.0f}, {"last_event", "Last Event", 125.0f},
});

static_assert(std::ranges::none_of(
    kPositionColumns, [](const TableColumnDefinition& definition) {
        return definition.id.empty() || definition.label.empty();
    }));
static_assert(std::ranges::none_of(
    kOrderColumns, [](const TableColumnDefinition& definition) {
        return definition.id.empty() || definition.label.empty();
    }));

const TableColumnDefinition* FindColumn(
    std::span<const TableColumnDefinition> definitions, std::string_view id) {
    const auto found = std::ranges::find(definitions, id,
                                         &TableColumnDefinition::id);
    return found == definitions.end() ? nullptr : &*found;
}

std::expected<void, std::string> ChangeColumn(
    WorkspaceState& state, std::string_view window_id,
    std::string_view table_id, std::span<const TableColumnDefinition> definitions,
    std::string_view id, bool add) {
    const auto window = state.windows.find(std::string(window_id));
    if (window == state.windows.end())
        return std::unexpected("Window has not been created");
    const TableColumnDefinition* definition = FindColumn(definitions, id);
    if (definition == nullptr) return std::unexpected("Unknown table column");
    PersistentTableState& table = window->second.tables[std::string(table_id)];
    const auto found = std::ranges::find(table.columns, id, &ColumnState::id);
    if (add) {
        if (found != table.columns.end()) {
            found->visible = true;
            return {};
        }
        const int next_order = table.columns.empty()
                                   ? 0
                                   : std::ranges::max(table.columns, {},
                                                      &ColumnState::order).order + 1;
        table.columns.push_back({.id = std::string(definition->id),
                                 .order = next_order,
                                 .width = definition->default_width,
                                 .visible = true});
        return {};
    }
    if (definition->required)
        return std::unexpected("This column is required");
    if (found == table.columns.end()) return {};
    found->visible = false;
    return {};
}

std::expected<void, std::string> CreateWindow(
    WorkspaceState& state, std::string_view id, std::string_view kind,
    std::string_view title, std::string_view table_id,
    std::initializer_list<std::string_view> columns, LogicalRect bounds) {
    if (const auto found = state.windows.find(std::string(id));
        found != state.windows.end()) {
        if (found->second.kind != kind)
            return std::unexpected("Window ID is owned by another window");
        found->second.open = true;
        if (!found->second.tables.contains(std::string(table_id))) {
            int order = 0;
            for (const std::string_view column : columns)
                found->second.tables[std::string(table_id)].columns.push_back(
                    {.id = std::string(column), .order = order++,
                     .width = 125.0f, .visible = true});
        }
        return {};
    }
    WindowInstanceState window{.id = std::string(id), .kind = std::string(kind),
                               .title = std::string(title), .open = true,
                               .bounds = bounds};
    int order = 0;
    for (const std::string_view column : columns)
        window.tables[std::string(table_id)].columns.push_back(
            {.id = std::string(column), .order = order++, .width = 125.0f,
             .visible = true});
    state.windows.emplace(window.id, std::move(window));
    return {};
}
}  // namespace

std::span<const TableColumnDefinition> PositionColumnDefinitions() {
    return kPositionColumns;
}

std::span<const TableColumnDefinition> OrderColumnDefinitions() {
    return kOrderColumns;
}

const TableColumnDefinition* FindPositionColumn(std::string_view id) {
    return FindColumn(kPositionColumns, id);
}

const TableColumnDefinition* FindOrderColumn(std::string_view id) {
    return FindColumn(kOrderColumns, id);
}

std::expected<void, std::string> CreatePositionsWindow(WorkspaceState& state) {
    return CreateWindow(state, kPositionsWindowId, "positions", "Positions",
                        kPositionsTableId,
                        {"symbol", "qty", "avg_entry", "last", "market_value",
                         "pnl", "pnl_percent"},
                        {820.0f, 330.0f, 760.0f, 300.0f});
}
std::expected<void, std::string> CreateOrdersWindow(WorkspaceState& state) {
    const auto created = CreateWindow(state, kOrdersWindowId, "orders", "Orders", kOrdersTableId,
                        {"symbol", "side", "status", "class", "qty", "filled",
                         "filled_avg", "filled_value", "limit", "stop", "submitted"},
                        {820.0f, 650.0f, 760.0f, 300.0f});
    if (!created) return created;
    PersistentTableState& table = state.windows.at(std::string(kOrdersWindowId))
                                      .tables.at(std::string(kOrdersTableId));
    if (std::ranges::none_of(table.columns, [](const ColumnState& column) {
            return !column.sort_direction.empty();
        })) {
        const auto symbol = std::ranges::find(table.columns, "symbol",
                                               &ColumnState::id);
        if (symbol != table.columns.end()) symbol->sort_direction = "ascending";
    }
    return {};
}

std::expected<void, std::string> AddPositionColumn(
    WorkspaceState& state, std::string_view id) {
    return ChangeColumn(state, kPositionsWindowId, kPositionsTableId,
                        kPositionColumns, id, true);
}

std::expected<void, std::string> RemovePositionColumn(
    WorkspaceState& state, std::string_view id) {
    return ChangeColumn(state, kPositionsWindowId, kPositionsTableId,
                        kPositionColumns, id, false);
}

std::expected<void, std::string> AddOrderColumn(
    WorkspaceState& state, std::string_view id) {
    return ChangeColumn(state, kOrdersWindowId, kOrdersTableId,
                        kOrderColumns, id, true);
}

std::expected<void, std::string> RemoveOrderColumn(
    WorkspaceState& state, std::string_view id) {
    return ChangeColumn(state, kOrdersWindowId, kOrdersTableId,
                        kOrderColumns, id, false);
}
}  // namespace tradebox::workstation
