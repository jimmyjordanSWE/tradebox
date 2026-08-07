#include "positions_window.h"

#include "tradebox/workstation/positions_orders_windows.h"

#include "imgui.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace tradebox::gui {
namespace {

std::string Money(const core::Decimal& value) {
    return std::format("${:.2f}", value.ToDisplayDouble());
}

std::string SignedMoney(const core::Decimal& value) {
    return std::format("{}${:.2f}", value > core::Decimal::Zero() ? "+" : "",
                       value.ToDisplayDouble());
}

std::string SignedPercent(const core::Decimal& value) {
    return std::format("{}{:.2f}%", value > core::Decimal::Zero() ? "+" : "",
                       value.ToDisplayDouble() * 100.0);
}

std::string_view FeedName(core::MarketDataFeed feed) {
    switch (feed) {
        case core::MarketDataFeed::Iex: return "IEX";
        case core::MarketDataFeed::Sip: return "SIP";
        case core::MarketDataFeed::Unknown: return "Unknown";
    }
    return "Unknown";
}

void DrawPnl(const core::Decimal& value, std::string_view text) {
    const ImVec4 color = value > core::Decimal::Zero()
                             ? ImVec4{0.30f, 0.85f, 0.40f, 1.0f}
                             : value < core::Decimal::Zero()
                                   ? ImVec4{0.95f, 0.32f, 0.32f, 1.0f}
                                   : ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::TextColored(color, "%.*s", static_cast<int>(text.size()), text.data());
}

bool IsCompletedOrder(std::string_view status) {
    return status == "filled" || status == "canceled" ||
           status == "expired" || status == "rejected" || status == "failed";
}

std::vector<workstation::ColumnState*> OrderedColumns(
    workstation::PersistentTableState& table) {
    std::vector<workstation::ColumnState*> columns;
    columns.reserve(table.columns.size());
    for (workstation::ColumnState& column : table.columns)
        if (column.visible) columns.push_back(&column);
    std::stable_sort(columns.begin(), columns.end(),
                     [](const auto* left, const auto* right) {
        if (left->order != right->order) return left->order < right->order;
        return left->id < right->id;
    });
    return columns;
}

std::string ColumnIdFromTableName(const char* name) {
    if (name == nullptr) return {};
    const std::string_view value(name);
    const std::size_t separator = value.find("###");
    return std::string(value.substr(
        separator == std::string_view::npos ? 0 : separator + 3));
}

template <typename FindColumn>
void SetupColumns(const std::vector<workstation::ColumnState*>& columns,
                  FindColumn&& find_column) {
    for (const auto* column : columns) {
        const auto* definition = find_column(column->id);
        const std::string label =
            std::string(definition == nullptr ? column->id : definition->label) +
            "###" + column->id;
        ImGuiTableColumnFlags flags = ImGuiTableColumnFlags_WidthFixed;
        if (column->sort_direction == "descending")
            flags |= ImGuiTableColumnFlags_DefaultSort |
                     ImGuiTableColumnFlags_PreferSortDescending;
        else if (column->sort_direction == "ascending")
            flags |= ImGuiTableColumnFlags_DefaultSort;
        ImGui::TableSetupColumn(
            label.c_str(), flags,
            column->width > 0.0f ? column->width : 125.0f);
    }
}

template <typename Definitions, typename Category>
void DrawColumnContextMenu(
    workstation::PersistentTableState& table, Definitions definitions,
    const workstation::TableColumnDefinition* current,
    std::optional<std::string>& pending_remove,
    std::optional<std::string>& pending_add, Category&& category) {
    const std::string popup_id = "column-context###" +
                                 std::string(current == nullptr
                                                 ? "unknown"
                                                 : current->id);
    if (!ImGui::BeginPopupContextItem(
            popup_id.c_str(), ImGuiPopupFlags_MouseButtonRight))
        return;
    if (current != nullptr && !current->required &&
        ImGui::MenuItem("Remove column"))
        pending_remove = std::string(current->id);
    ImGui::SeparatorText("Add column");
    ImGui::SetNextWindowSizeConstraints({300.0f, 0.0f}, {440.0f, 420.0f});
    if (ImGui::BeginChild("available-columns", {0.0f, 300.0f},
                          ImGuiChildFlags_Borders)) {
        std::string_view previous_category;
        for (const workstation::TableColumnDefinition& definition : definitions) {
            const bool present = std::ranges::find(
                table.columns, definition.id, &workstation::ColumnState::id) !=
                                 table.columns.end();
            if (present) continue;
            const std::string_view next_category = category(definition.id);
            if (next_category != previous_category) {
                ImGui::SeparatorText(next_category.data());
                previous_category = next_category;
            }
            bool add = false;
            if (ImGui::Checkbox(definition.label.data(), &add) && add) {
                pending_add = std::string(definition.id);
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::EndChild();
    if (pending_add) {
        ImGui::TextDisabled("Column will be added on close.");
    }
    ImGui::EndPopup();
}

std::string_view PositionColumnCategory(std::string_view id) {
    if (id == "symbol" || id == "side" || id == "exchange" ||
        id == "asset_class" || id == "asset_id")
        return "Instrument";
    if (id == "qty" || id == "qty_available" || id == "avg_entry" ||
        id == "last" || id == "prev_close" || id == "market_value" ||
        id == "cost_basis" || id == "pnl" || id == "pnl_percent" ||
        id == "day_pnl" || id == "day_pnl_percent" || id == "change_today")
        return "Market and P&L";
    return "Valuation";
}

std::string_view OrderColumnCategory(std::string_view id) {
    if (id == "symbol" || id == "side" || id == "status" ||
        id == "type" || id == "tif" || id == "class" ||
        id == "extended_hours")
        return "Order";
    if (id == "qty" || id == "notional" || id == "filled" ||
        id == "filled_avg" || id == "limit" || id == "stop")
        return "Quantity and Price";
    if (id == "submitted" || id == "updated" || id == "filled_at" ||
        id == "canceled_at" || id == "expired_at" || id == "failed_at" ||
        id == "replaced_at" || id == "last_event")
        return "Lifecycle";
    return "Identity and Relationship";
}

void StoreSortDirection(workstation::PersistentTableState& table,
                        const ImGuiTableSortSpecs* sort_specs,
                        bool& changed) {
    if (sort_specs == nullptr || sort_specs->SpecsCount == 0) return;
    const ImGuiTableColumnSortSpecs& spec = sort_specs->Specs[0];
    const std::string id = ColumnIdFromTableName(
        ImGui::TableGetColumnName(spec.ColumnIndex));
    const std::string direction =
        spec.SortDirection == ImGuiSortDirection_Descending ? "descending"
                                                            : "ascending";
    for (workstation::ColumnState& column : table.columns) {
        const std::string next = column.id == id ? direction : "";
        if (column.sort_direction != next) {
            column.sort_direction = next;
            changed = true;
        }
    }
}

std::optional<core::Decimal> PositionNumber(
    const core::PositionState& position, std::string_view column) {
    if (column == "qty") return position.qty;
    if (column == "qty_available") return position.qty_available;
    if (column == "avg_entry") return position.avg_entry_price;
    if (column == "last") return position.current_price;
    if (column == "prev_close") return position.lastday_price;
    if (column == "market_value") return position.market_value;
    if (column == "cost_basis") return position.cost_basis;
    if (column == "pnl") return position.unrealized_pl;
    if (column == "pnl_percent") return position.unrealized_plpc;
    if (column == "day_pnl") return position.unrealized_intraday_pl;
    if (column == "day_pnl_percent") return position.unrealized_intraday_plpc;
    if (column == "change_today") return position.change_today;
    return std::nullopt;
}

std::string_view PositionText(const core::PositionState& position,
                              std::string_view column) {
    if (column == "symbol") return position.symbol;
    if (column == "side") return position.side;
    if (column == "exchange") return position.exchange;
    if (column == "asset_class") return position.asset_class;
    if (column == "asset_id") return position.asset_id;
    if (column == "provisional") return position.provisional ? "Yes" : "No";
    if (column == "valuation_current")
        return position.valuation_current ? "Yes" : "No";
    if (column == "valuation_stream")
        return position.valuation_from_market_stream ? "Yes" : "No";
    if (column == "valuation_feed") return FeedName(position.valuation_feed);
    if (column == "valuation")
        return position.valuation_current ? "Current" : "Stale";
    return {};
}

std::optional<core::Decimal> OrderNumber(const core::OrderState& order,
                                         std::string_view column) {
    if (column == "qty") return order.qty;
    if (column == "notional") return order.notional;
    if (column == "filled") return order.filled_qty;
    if (column == "filled_avg") return order.filled_avg_price;
    if (column == "limit") return order.limit_price;
    if (column == "stop") return order.stop_price;
    return std::nullopt;
}

std::string_view OrderText(const core::OrderState& order,
                           std::string_view column) {
    if (column == "symbol") return order.symbol;
    if (column == "side") return order.side;
    if (column == "status") return order.status;
    if (column == "type") return order.type;
    if (column == "tif") return order.time_in_force;
    if (column == "class") return order.order_class;
    if (column == "submitted") return order.submitted_at;
    if (column == "updated") return order.updated_at;
    if (column == "client_order_id") return order.client_order_id;
    if (column == "order_id") return order.id;
    if (column == "parent_order_id") return order.parent_order_id;
    if (column == "asset_id") return order.asset_id;
    if (column == "asset_class") return order.asset_class;
    if (column == "filled_at") return order.filled_at;
    if (column == "canceled_at") return order.canceled_at;
    if (column == "expired_at") return order.expired_at;
    if (column == "failed_at") return order.failed_at;
    if (column == "replaced_at") return order.replaced_at;
    if (column == "replaced_by") return order.replaced_by;
    if (column == "replaces") return order.replaces;
    if (column == "last_event") return order.last_event;
    return {};
}

}  // namespace

void PositionsWindowRenderer::AppendSnapshotQuery(
    workstation::WorkspaceState& state, application::UiSnapshotQuery& query) {
    const auto found = state.windows.find(
        std::string(workstation::kPositionsWindowId));
    query.include_position_markets =
        found != state.windows.end() && found->second.open;
}

void PositionsWindowRenderer::Draw(
    ui::Workspace& workspace, workstation::WorkspaceState& state,
    const application::ApplicationUiSnapshot& snapshot) {
    const auto found = state.windows.find(
        std::string(workstation::kPositionsWindowId));
    if (found == state.windows.end() || !found->second.open) return;
    workstation::WindowInstanceState& persisted = found->second;

    ui::WorkspaceWindow window{
        .title = "Positions",
        .id = std::string(workstation::kPositionsWindowId),
        .default_offset = {820.0f, 330.0f},
        .default_size = {760.0f, 300.0f},
        .open = true,
        .flags = ImGuiWindowFlags_NoCollapse,
    };
    if (!workspace.BeginWindow(window)) {
        workspace.EndWindow(window);
        return;
    }

    auto& table = persisted.tables[std::string(workstation::kPositionsTableId)];
    const auto columns = OrderedColumns(table);
    std::optional<std::string> pending_remove;
    std::optional<std::string> pending_add;
    if (ImGui::BeginTable("positions", static_cast<int>(columns.size()),
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Sortable |
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_NoSavedSettings,
                          {0.0f, 0.0f})) {
        SetupColumns(columns, workstation::FindPositionColumn);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (const auto* column : columns) {
            ImGui::TableNextColumn();
            const auto* definition = workstation::FindPositionColumn(column->id);
            ImGui::TableHeader(definition == nullptr ? column->id.c_str()
                                                     : definition->label.data());
            DrawColumnContextMenu(table,
                                  workstation::PositionColumnDefinitions(),
                                  definition, pending_remove, pending_add,
                                  PositionColumnCategory);
        }
        const ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs();
        StoreSortDirection(table, sort_specs, persistent_changed_);
        std::vector<const core::PositionState*> rows;
        rows.reserve(snapshot.core.positions.size());
        for (const core::PositionState& position : snapshot.core.positions)
            rows.push_back(&position);
        if (sort_specs != nullptr && sort_specs->SpecsCount > 0) {
            const std::string column = ColumnIdFromTableName(
                ImGui::TableGetColumnName(sort_specs->Specs[0].ColumnIndex));
            const bool descending = sort_specs->Specs[0].SortDirection ==
                                    ImGuiSortDirection_Descending;
            std::stable_sort(rows.begin(), rows.end(),
                             [&](const auto* left, const auto* right) {
                const auto left_number = PositionNumber(*left, column);
                const auto right_number = PositionNumber(*right, column);
                if (left_number && right_number && *left_number != *right_number)
                    return descending ? *left_number > *right_number
                                      : *left_number < *right_number;
                if (left_number.has_value() != right_number.has_value())
                    return left_number.has_value();
                const auto left_text = PositionText(*left, column);
                const auto right_text = PositionText(*right, column);
                if (left_text != right_text)
                    return descending ? left_text > right_text : left_text < right_text;
                return left->symbol < right->symbol;
            });
        }
        for (const core::PositionState* position : rows) {
            ImGui::TableNextRow();
            for (const auto* column : columns) {
                ImGui::TableNextColumn();
                if (column->id == "symbol") ImGui::TextUnformatted(position->symbol.c_str());
                else if (column->id == "qty") ImGui::TextUnformatted(position->qty.ToString().c_str());
                else if (column->id == "qty_available") ImGui::TextUnformatted(position->qty_available.ToString().c_str());
                else if (column->id == "side") ImGui::TextUnformatted(position->side.c_str());
                else if (column->id == "avg_entry") ImGui::TextUnformatted(Money(position->avg_entry_price).c_str());
                else if (column->id == "last") ImGui::TextUnformatted(Money(position->current_price).c_str());
                else if (column->id == "prev_close") ImGui::TextUnformatted(Money(position->lastday_price).c_str());
                else if (column->id == "market_value") ImGui::TextUnformatted(Money(position->market_value).c_str());
                else if (column->id == "cost_basis") ImGui::TextUnformatted(Money(position->cost_basis).c_str());
                else if (column->id == "pnl") DrawPnl(position->unrealized_pl, SignedMoney(position->unrealized_pl));
                else if (column->id == "pnl_percent") DrawPnl(position->unrealized_plpc, SignedPercent(position->unrealized_plpc));
                else if (column->id == "day_pnl") DrawPnl(position->unrealized_intraday_pl, SignedMoney(position->unrealized_intraday_pl));
                else if (column->id == "day_pnl_percent") DrawPnl(position->unrealized_intraday_plpc, SignedPercent(position->unrealized_intraday_plpc));
                else if (column->id == "change_today") DrawPnl(position->change_today, SignedPercent(position->change_today));
                else if (column->id == "exchange") ImGui::TextUnformatted(position->exchange.c_str());
                else if (column->id == "asset_class") ImGui::TextUnformatted(position->asset_class.c_str());
                else if (column->id == "valuation") ImGui::TextUnformatted(position->valuation_current ? "Current" : "Stale");
                else if (column->id == "asset_id") ImGui::TextUnformatted(position->asset_id.c_str());
                else if (column->id == "provisional") ImGui::TextUnformatted(position->provisional ? "Yes" : "No");
                else if (column->id == "valuation_current") ImGui::TextUnformatted(position->valuation_current ? "Yes" : "No");
                else if (column->id == "valuation_stream") ImGui::TextUnformatted(position->valuation_from_market_stream ? "Yes" : "No");
                else if (column->id == "valuation_feed") ImGui::TextUnformatted(FeedName(position->valuation_feed).data());
                else if (column->id == "valuation_event_ns") ImGui::Text("%lld", static_cast<long long>(position->valuation_event_time_ns));
                else if (column->id == "valuation_received_ms") ImGui::Text("%lld", static_cast<long long>(position->valuation_received_at_ms));
            }
        }
        ImGui::EndTable();
    }
    if (pending_remove) {
        const auto removed = workstation::RemovePositionColumn(
            state, *pending_remove);
        if (removed) persistent_changed_ = true;
    }
    if (pending_add) {
        const auto added = workstation::AddPositionColumn(state, *pending_add);
        if (added) persistent_changed_ = true;
    }
    workspace.EndWindow(window);
}

bool PositionsWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

void OrdersWindowRenderer::Draw(
    ui::Workspace& workspace, workstation::WorkspaceState& state,
    const application::ApplicationUiSnapshot& snapshot) {
    const auto found = state.windows.find(
        std::string(workstation::kOrdersWindowId));
    if (found == state.windows.end() || !found->second.open) return;
    workstation::WindowInstanceState& persisted = found->second;
    ui::WorkspaceWindow window{
        .title = "Orders", .id = std::string(workstation::kOrdersWindowId),
        .default_offset = {820.0f, 650.0f}, .default_size = {760.0f, 300.0f},
        .open = true, .flags = ImGuiWindowFlags_NoCollapse,
    };
    if (!workspace.BeginWindow(window)) {
        workspace.EndWindow(window);
        return;
    }
    if (ImGui::Checkbox("Active##orders", &state.show_active_orders))
        persistent_changed_ = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Completed##orders", &state.show_filled_orders))
        persistent_changed_ = true;
    auto& table = persisted.tables[std::string(workstation::kOrdersTableId)];
    const auto columns = OrderedColumns(table);
    std::optional<std::string> pending_remove;
    std::optional<std::string> pending_add;
    if (ImGui::BeginTable("orders", static_cast<int>(columns.size()),
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Sortable |
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_NoSavedSettings,
                          {0.0f, 0.0f})) {
        SetupColumns(columns, workstation::FindOrderColumn);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (const auto* column : columns) {
            ImGui::TableNextColumn();
            const auto* definition = workstation::FindOrderColumn(column->id);
            ImGui::TableHeader(definition == nullptr ? column->id.c_str()
                                                     : definition->label.data());
            DrawColumnContextMenu(table, workstation::OrderColumnDefinitions(),
                                  definition, pending_remove, pending_add,
                                  OrderColumnCategory);
        }
        const ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs();
        StoreSortDirection(table, sort_specs, persistent_changed_);
        std::vector<const core::OrderState*> rows;
        rows.reserve(snapshot.core.orders.size());
        for (const core::OrderState& order : snapshot.core.orders) {
            const bool completed = IsCompletedOrder(order.status);
            if ((completed && !state.show_filled_orders) ||
                (!completed && !state.show_active_orders))
                continue;
            rows.push_back(&order);
        }
        if (sort_specs != nullptr && sort_specs->SpecsCount > 0) {
            const std::string column = ColumnIdFromTableName(
                ImGui::TableGetColumnName(sort_specs->Specs[0].ColumnIndex));
            const bool descending = sort_specs->Specs[0].SortDirection ==
                                    ImGuiSortDirection_Descending;
            std::stable_sort(rows.begin(), rows.end(),
                             [&](const auto* left, const auto* right) {
                const auto left_number = OrderNumber(*left, column);
                const auto right_number = OrderNumber(*right, column);
                if (left_number && right_number && *left_number != *right_number)
                    return descending ? *left_number > *right_number
                                      : *left_number < *right_number;
                if (left_number.has_value() != right_number.has_value())
                    return left_number.has_value();
                const auto left_text = OrderText(*left, column);
                const auto right_text = OrderText(*right, column);
                if (left_text != right_text)
                    return descending ? left_text > right_text : left_text < right_text;
                return left->id < right->id;
            });
        } else {
            std::stable_sort(rows.begin(), rows.end(),
                             [](const auto* left, const auto* right) {
                if (left->parent_order_id.empty() != right->parent_order_id.empty())
                    return left->parent_order_id.empty();
                if (left->parent_order_id != right->parent_order_id)
                    return left->parent_order_id < right->parent_order_id;
                return left->submitted_at_ms > right->submitted_at_ms;
            });
        }
        for (const core::OrderState* order : rows) {
            ImGui::TableNextRow();
            for (const auto* column : columns) {
                ImGui::TableNextColumn();
                if (column->id == "symbol") {
                    if (!order->parent_order_id.empty()) ImGui::Indent();
                    ImGui::TextUnformatted(order->symbol.c_str());
                    if (!order->parent_order_id.empty()) ImGui::Unindent();
                } else if (column->id == "side") ImGui::TextUnformatted(order->side.c_str());
                else if (column->id == "status") ImGui::TextUnformatted(order->status.c_str());
                else if (column->id == "type") ImGui::TextUnformatted(order->type.c_str());
                else if (column->id == "qty") ImGui::TextUnformatted(order->qty ? order->qty->ToString().c_str() : "--");
                else if (column->id == "notional") ImGui::TextUnformatted(order->notional ? Money(*order->notional).c_str() : "--");
                else if (column->id == "filled") ImGui::TextUnformatted(order->filled_qty.ToString().c_str());
                else if (column->id == "filled_avg") ImGui::TextUnformatted(order->filled_avg_price ? Money(*order->filled_avg_price).c_str() : "--");
                else if (column->id == "limit") ImGui::TextUnformatted(order->limit_price ? Money(*order->limit_price).c_str() : "--");
                else if (column->id == "stop") ImGui::TextUnformatted(order->stop_price ? Money(*order->stop_price).c_str() : "--");
                else if (column->id == "tif") ImGui::TextUnformatted(order->time_in_force.c_str());
                else if (column->id == "class") ImGui::TextUnformatted(order->order_class.c_str());
                else if (column->id == "extended_hours") ImGui::TextUnformatted(order->extended_hours ? "Yes" : "No");
                else if (column->id == "submitted") ImGui::TextUnformatted(order->submitted_at.c_str());
                else if (column->id == "updated") ImGui::TextUnformatted(order->updated_at.c_str());
                else if (column->id == "client_order_id") ImGui::TextUnformatted(order->client_order_id.c_str());
                else if (column->id == "order_id") ImGui::TextUnformatted(order->id.c_str());
                else if (column->id == "parent_order_id") ImGui::TextUnformatted(order->parent_order_id.c_str());
                else if (column->id == "asset_id") ImGui::TextUnformatted(order->asset_id.c_str());
                else if (column->id == "asset_class") ImGui::TextUnformatted(order->asset_class.c_str());
                else if (column->id == "filled_at") ImGui::TextUnformatted(order->filled_at.c_str());
                else if (column->id == "canceled_at") ImGui::TextUnformatted(order->canceled_at.c_str());
                else if (column->id == "expired_at") ImGui::TextUnformatted(order->expired_at.c_str());
                else if (column->id == "failed_at") ImGui::TextUnformatted(order->failed_at.c_str());
                else if (column->id == "replaced_at") ImGui::TextUnformatted(order->replaced_at.c_str());
                else if (column->id == "replaced_by") ImGui::TextUnformatted(order->replaced_by.c_str());
                else if (column->id == "replaces") ImGui::TextUnformatted(order->replaces.c_str());
                else if (column->id == "last_event") ImGui::TextUnformatted(order->last_event.c_str());
            }
        }
        ImGui::EndTable();
    }

    if (pending_remove) {
        const auto removed = workstation::RemoveOrderColumn(state,
                                                              *pending_remove);
        if (removed) persistent_changed_ = true;
    }
    if (pending_add) {
        const auto added = workstation::AddOrderColumn(state, *pending_add);
        if (added) persistent_changed_ = true;
    }

    workspace.EndWindow(window);
}

bool OrdersWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

}  // namespace tradebox::gui
