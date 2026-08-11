#include "positions_window.h"

#include "tradebox/core/order_projection.h"
#include "tradebox/workstation/positions_orders_windows.h"

#include "gui_controls.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
           status == "cancelled" || status == "expired" ||
           status == "rejected" || status == "failed" ||
           status == "done_for_day" || status == "stopped";
}

constexpr std::array kTimeInForceChoices{
    std::pair{"day", core::TimeInForce::Day},
    std::pair{"gtc", core::TimeInForce::Gtc},
    std::pair{"opg", core::TimeInForce::Opg},
    std::pair{"cls", core::TimeInForce::Cls},
    std::pair{"ioc", core::TimeInForce::Ioc},
    std::pair{"fok", core::TimeInForce::Fok},
};

int TimeInForceIndex(std::string_view value) {
    for (std::size_t index = 0; index < kTimeInForceChoices.size(); ++index)
        if (value == kTimeInForceChoices[index].first)
            return static_cast<int>(index);
    return 0;
}

bool IsActiveOrder(const core::OrderState& order) {
    return !IsCompletedOrder(order.status);
}

void SetDecimalText(std::array<char, 64>& output,
                    const std::optional<core::Decimal>& value) {
    if (!value) return;
    const std::string text = value->ToString();
    std::snprintf(output.data(), output.size(), "%s", text.c_str());
}

std::optional<core::Decimal> ParseReplacementDecimal(
    const std::array<char, 64>& value, std::string_view label,
    std::string& error) {
    if (value.front() == '\0') {
        error = std::string(label) + " is required";
        return std::nullopt;
    }
    const auto parsed = core::Decimal::Parse(value.data());
    if (!parsed) {
        error = std::string(label) + " must be a valid decimal";
        return std::nullopt;
    }
    return *parsed;
}

std::vector<TableColumnChoice> TableColumnChoices(
    std::span<const workstation::TableColumnDefinition> definitions) {
    std::vector<TableColumnChoice> choices;
    choices.reserve(definitions.size());
    for (const workstation::TableColumnDefinition& definition : definitions) {
        choices.push_back({definition.id, definition.label,
                           definition.required});
    }
    return choices;
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
    if (column == "filled_value") return core::FilledOrderValue(order);
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
    if (pending_exit_result_ &&
        pending_exit_result_->wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        exit_result_ = pending_exit_result_->get();
        pending_exit_result_.reset();
    }
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (!workspace.BeginWindow(window)) {
        workspace.EndWindow(window);
        ImGui::PopStyleVar();
        return;
    }
    if (pending_exit_result_)
        ImGui::TextDisabled("Exit request pending...");
    else if (exit_result_)
        ImGui::TextColored(
            exit_result_->AcceptedByBroker() ? ImVec4{.30f, .85f, .40f, 1.0f}
                                              : ImVec4{.95f, .32f, .32f, 1.0f},
            "%s", exit_result_->AcceptedByBroker()
                       ? "Exit accepted; reconciling position state."
                       : exit_result_->message.c_str());

    auto& table = persisted.tables[std::string(workstation::kPositionsTableId)];
    const auto choices = TableColumnChoices(
        workstation::PositionColumnDefinitions());
    const auto columns = OrderedTableColumns(table);
    const TableColumnActions column_actions = DrawTableColumnControls(
        table, choices, "positions_columns");
    if (ImGui::BeginTable("positions", static_cast<int>(columns.size()),
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              PersistentTableInteractionFlags(),
                          {0.0f, 0.0f})) {
        SetupPersistentTableColumns(columns, choices, 125.0f);
        ImGui::TableHeadersRow();
        const std::vector<std::string> display_column_ids =
            CurrentVisibleTableColumnIds();
        const ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs();
        persistent_changed_ =
            PersistTableSortSpecs(table, sort_specs) || persistent_changed_;
        std::vector<const core::PositionState*> rows;
        rows.reserve(snapshot.core.positions.size());
        for (const core::PositionState& position : snapshot.core.positions)
            rows.push_back(&position);
        if (sort_specs != nullptr && sort_specs->SpecsCount > 0) {
            const std::string column = TableColumnIdFromLabel(
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
            for (const std::string& column : display_column_ids) {
                ImGui::TableNextColumn();
                if (column == "symbol") {
                    ImGui::TextUnformatted(position->symbol.c_str());
                    ImGui::SameLine();
                    const bool can_exit = position->asset_class == "us_equity";
                    if (!can_exit) ImGui::BeginDisabled();
                    const std::string exit_button_id = std::format(
                        "X##exit-position-{}", position->asset_id);
                    if (ImGui::SmallButton(exit_button_id.c_str())) {
                        exit_confirmation_symbol_ = position->symbol;
                        ImGui::OpenPopup("Exit position?##positions");
                    }
                    if (!can_exit) {
                        ImGui::EndDisabled();
                        ImGui::SetItemTooltip(
                            "Exit from Positions currently supports US equities only.");
                    } else {
                        ImGui::SetItemTooltip(
                            "Exit this entire position at market.");
                    }
                }
                else if (column == "qty") ImGui::TextUnformatted(position->qty.ToString().c_str());
                else if (column == "qty_available") ImGui::TextUnformatted(position->qty_available.ToString().c_str());
                else if (column == "side") ImGui::TextUnformatted(position->side.c_str());
                else if (column == "avg_entry") ImGui::TextUnformatted(Money(position->avg_entry_price).c_str());
                else if (column == "last") ImGui::TextUnformatted(Money(position->current_price).c_str());
                else if (column == "prev_close") ImGui::TextUnformatted(Money(position->lastday_price).c_str());
                else if (column == "market_value") ImGui::TextUnformatted(Money(position->market_value).c_str());
                else if (column == "cost_basis") ImGui::TextUnformatted(Money(position->cost_basis).c_str());
                else if (column == "pnl") DrawPnl(position->unrealized_pl, SignedMoney(position->unrealized_pl));
                else if (column == "pnl_percent") DrawPnl(position->unrealized_plpc, SignedPercent(position->unrealized_plpc));
                else if (column == "day_pnl") DrawPnl(position->unrealized_intraday_pl, SignedMoney(position->unrealized_intraday_pl));
                else if (column == "day_pnl_percent") DrawPnl(position->unrealized_intraday_plpc, SignedPercent(position->unrealized_intraday_plpc));
                else if (column == "change_today") DrawPnl(position->change_today, SignedPercent(position->change_today));
                else if (column == "exchange") ImGui::TextUnformatted(position->exchange.c_str());
                else if (column == "asset_class") ImGui::TextUnformatted(position->asset_class.c_str());
                else if (column == "valuation") ImGui::TextUnformatted(position->valuation_current ? "Current" : "Stale");
                else if (column == "asset_id") ImGui::TextUnformatted(position->asset_id.c_str());
                else if (column == "provisional") ImGui::TextUnformatted(position->provisional ? "Yes" : "No");
                else if (column == "valuation_current") ImGui::TextUnformatted(position->valuation_current ? "Yes" : "No");
                else if (column == "valuation_stream") ImGui::TextUnformatted(position->valuation_from_market_stream ? "Yes" : "No");
                else if (column == "valuation_feed") ImGui::TextUnformatted(FeedName(position->valuation_feed).data());
                else if (column == "valuation_event_ns") ImGui::Text("%lld", static_cast<long long>(position->valuation_event_time_ns));
                else if (column == "valuation_received_ms") ImGui::Text("%lld", static_cast<long long>(position->valuation_received_at_ms));
            }
        }
        persistent_changed_ =
            PersistCurrentTableLayout(table) ||
            persistent_changed_;
        ImGui::EndTable();
    }
    if (column_actions.add &&
        workstation::AddPositionColumn(state, *column_actions.add))
        persistent_changed_ = true;
    if (exit_confirmation_symbol_ &&
        ImGui::BeginPopupModal("Exit position?##positions", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Exit %s?", exit_confirmation_symbol_->c_str());
        ImGui::TextUnformatted(
            "This submits a market order to close the entire position.");
        ImGui::Separator();
        if (ImGui::Button("Exit Position", {120.0f, 0.0f})) {
            exit_request_ = *exit_confirmation_symbol_;
            exit_confirmation_symbol_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100.0f, 0.0f})) {
            exit_confirmation_symbol_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    workspace.EndWindow(window);
    ImGui::PopStyleVar();
}

std::optional<std::string> PositionsWindowRenderer::ConsumeExitRequest() {
    return std::exchange(exit_request_, std::nullopt);
}

void PositionsWindowRenderer::SetExitResult(
    std::future<core::OrderCommandResult> result) {
    pending_exit_result_ = std::move(result);
    exit_result_.reset();
}

bool PositionsWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

void OrdersWindowRenderer::Draw(
    ui::Workspace& workspace, workstation::WorkspaceState& state,
    const application::ApplicationUiSnapshot& snapshot) {
    if (pending_result_ &&
        pending_result_->wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        result_ = pending_result_->get();
        pending_result_.reset();
    }
    const auto found = state.windows.find(
        std::string(workstation::kOrdersWindowId));
    if (found == state.windows.end() || !found->second.open) return;
    workstation::WindowInstanceState& persisted = found->second;
    ui::WorkspaceWindow window{
        .title = "Orders", .id = std::string(workstation::kOrdersWindowId),
        .default_offset = {820.0f, 650.0f}, .default_size = {760.0f, 300.0f},
        .open = true, .flags = ImGuiWindowFlags_NoCollapse,
    };
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (!workspace.BeginWindow(window)) {
        workspace.EndWindow(window);
        ImGui::PopStyleVar();
        return;
    }
    if (ImGui::Checkbox("Active##orders", &state.show_active_orders))
        persistent_changed_ = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Completed##orders", &state.show_filled_orders))
        persistent_changed_ = true;
    ImGui::SameLine();
    if (pending_result_)
        ImGui::TextDisabled("Order request pending...");
    else if (result_)
        ImGui::TextColored(
            result_->AcceptedByBroker() ? ImVec4{.30f, .85f, .40f, 1.0f}
                                        : ImVec4{.95f, .32f, .32f, 1.0f},
            "%s", result_->AcceptedByBroker()
                       ? "Order request accepted; reconciling order state."
                       : result_->message.c_str());
    auto& table = persisted.tables[std::string(workstation::kOrdersTableId)];
    const auto choices = TableColumnChoices(
        workstation::OrderColumnDefinitions());
    const auto columns = OrderedTableColumns(table);
    const TableColumnActions column_actions = DrawTableColumnControls(
        table, choices, "orders_columns");
    if (ImGui::BeginTable("orders", static_cast<int>(columns.size() + 1U),
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              PersistentTableInteractionFlags(),
                          {0.0f, 0.0f})) {
        ImGui::TableSetupColumn(
            "Actions", ImGuiTableColumnFlags_WidthFixed |
                           ImGuiTableColumnFlags_NoHide |
                           ImGuiTableColumnFlags_NoReorder |
                           ImGuiTableColumnFlags_NoSort,
            126.0f);
        SetupPersistentTableColumns(columns, choices, 125.0f, 1);
        ImGui::TableHeadersRow();
        const std::vector<std::string> display_column_ids =
            CurrentVisibleTableColumnIds(1);
        const ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs();
        persistent_changed_ =
            PersistTableSortSpecs(table, sort_specs) || persistent_changed_;
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
            const std::string column = TableColumnIdFromLabel(
                ImGui::TableGetColumnName(sort_specs->Specs[0].ColumnIndex));
            const bool descending = sort_specs->Specs[0].SortDirection ==
                                    ImGuiSortDirection_Descending;
            std::unordered_map<std::string, const core::OrderState*> roots;
            roots.reserve(rows.size());
            for (const core::OrderState* row : rows) {
                if (row->parent_order_id.empty()) roots.emplace(row->id, row);
            }
            std::stable_sort(rows.begin(), rows.end(),
                             [&](const auto* left, const auto* right) {
                if (column == "symbol") {
                    const auto root_for = [&](const core::OrderState* order) {
                        if (order->parent_order_id.empty()) return order;
                        const auto parent = roots.find(order->parent_order_id);
                        return parent == roots.end() ? order : parent->second;
                    };
                    const core::OrderState* const left_root = root_for(left);
                    const core::OrderState* const right_root = root_for(right);
                    if (left_root->symbol != right_root->symbol)
                        return descending ? left_root->symbol > right_root->symbol
                                          : left_root->symbol < right_root->symbol;
                    if (left_root->id != right_root->id)
                        return left_root->id < right_root->id;
                    if (left == left_root || right == right_root)
                        return left == left_root;
                    return left->id < right->id;
                }
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
            ImGui::TableNextColumn();
            const bool active = IsActiveOrder(*order);
            const bool bracket_leg = !order->parent_order_id.empty();
            if (!active || bracket_leg) ImGui::BeginDisabled();
            const std::string cancel_id = std::format("Cancel##order-{}", order->id);
            if (ImGui::SmallButton(cancel_id.c_str())) {
                cancel_confirmation_order_id_ = order->id;
                ImGui::OpenPopup("Cancel order?##orders");
            }
            ImGui::SameLine();
            const std::string replace_id = std::format("Replace##order-{}", order->id);
            if (ImGui::SmallButton(replace_id.c_str())) {
                ReplaceDraft draft{.order_id = order->id, .symbol = order->symbol};
                SetDecimalText(draft.qty, order->qty);
                SetDecimalText(draft.notional, order->notional);
                SetDecimalText(draft.limit_price, order->limit_price);
                SetDecimalText(draft.stop_price, order->stop_price);
                draft.time_in_force_index = TimeInForceIndex(order->time_in_force);
                replace_draft_ = std::move(draft);
                ImGui::OpenPopup("Replace order?##orders");
            }
            if (!active || bracket_leg) {
                ImGui::EndDisabled();
                ImGui::SetItemTooltip(bracket_leg
                    ? "Use the bracket group action to change this leg."
                    : "Terminal orders cannot be changed.");
            }
            for (const std::string& column : display_column_ids) {
                ImGui::TableNextColumn();
                if (column == "symbol") {
                    if (!order->parent_order_id.empty()) ImGui::Indent();
                    ImGui::TextUnformatted(order->symbol.c_str());
                    if (!order->parent_order_id.empty()) ImGui::Unindent();
                } else if (column == "side") ImGui::TextUnformatted(order->side.c_str());
                else if (column == "status") ImGui::TextUnformatted(order->status.c_str());
                else if (column == "type") ImGui::TextUnformatted(order->type.c_str());
                else if (column == "qty") ImGui::TextUnformatted(order->qty ? order->qty->ToString().c_str() : "--");
                else if (column == "notional") ImGui::TextUnformatted(order->notional ? Money(*order->notional).c_str() : "--");
                else if (column == "filled") ImGui::TextUnformatted(order->filled_qty.ToString().c_str());
                else if (column == "filled_avg") ImGui::TextUnformatted(order->filled_avg_price ? Money(*order->filled_avg_price).c_str() : "--");
                else if (column == "filled_value") {
                    const auto value = core::FilledOrderValue(*order);
                    ImGui::TextUnformatted(value ? Money(*value).c_str() : "--");
                }
                else if (column == "limit") ImGui::TextUnformatted(order->limit_price ? Money(*order->limit_price).c_str() : "--");
                else if (column == "stop") ImGui::TextUnformatted(order->stop_price ? Money(*order->stop_price).c_str() : "--");
                else if (column == "tif") ImGui::TextUnformatted(order->time_in_force.c_str());
                else if (column == "class") ImGui::TextUnformatted(order->order_class.c_str());
                else if (column == "extended_hours") ImGui::TextUnformatted(order->extended_hours ? "Yes" : "No");
                else if (column == "submitted") ImGui::TextUnformatted(order->submitted_at.c_str());
                else if (column == "updated") ImGui::TextUnformatted(order->updated_at.c_str());
                else if (column == "client_order_id") ImGui::TextUnformatted(order->client_order_id.c_str());
                else if (column == "order_id") ImGui::TextUnformatted(order->id.c_str());
                else if (column == "parent_order_id") ImGui::TextUnformatted(order->parent_order_id.c_str());
                else if (column == "asset_id") ImGui::TextUnformatted(order->asset_id.c_str());
                else if (column == "asset_class") ImGui::TextUnformatted(order->asset_class.c_str());
                else if (column == "filled_at") ImGui::TextUnformatted(order->filled_at.c_str());
                else if (column == "canceled_at") ImGui::TextUnformatted(order->canceled_at.c_str());
                else if (column == "expired_at") ImGui::TextUnformatted(order->expired_at.c_str());
                else if (column == "failed_at") ImGui::TextUnformatted(order->failed_at.c_str());
                else if (column == "replaced_at") ImGui::TextUnformatted(order->replaced_at.c_str());
                else if (column == "replaced_by") ImGui::TextUnformatted(order->replaced_by.c_str());
                else if (column == "replaces") ImGui::TextUnformatted(order->replaces.c_str());
                else if (column == "last_event") ImGui::TextUnformatted(order->last_event.c_str());
            }
        }
        persistent_changed_ =
            PersistCurrentTableLayout(table, 1) ||
            persistent_changed_;
        ImGui::EndTable();
    }
    if (column_actions.add &&
        workstation::AddOrderColumn(state, *column_actions.add))
        persistent_changed_ = true;

    if (cancel_confirmation_order_id_ &&
        ImGui::BeginPopupModal("Cancel order?##orders", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Cancel this open order?");
        ImGui::TextDisabled("The broker may still fill it before cancellation completes.");
        ImGui::Separator();
        if (ImGui::Button("Cancel Order", {120.0f, 0.0f})) {
            action_request_ = ActionRequest{
                .kind = ActionRequest::Kind::Cancel,
                .order_id = std::move(*cancel_confirmation_order_id_),
            };
            cancel_confirmation_order_id_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep Order", {100.0f, 0.0f})) {
            cancel_confirmation_order_id_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (replace_draft_ &&
        ImGui::BeginPopupModal("Replace order?##orders", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ReplaceDraft& draft = *replace_draft_;
        ImGui::Text("Replace %s", draft.symbol.c_str());
        ImGui::TextDisabled("Only checked fields will be changed.");
        const auto field = [](const char* label, bool& enabled,
                              std::array<char, 64>& text) {
            ImGui::Checkbox(label, &enabled);
            ImGui::SameLine();
            if (!enabled) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputText(std::format("##{}", label).c_str(), text.data(),
                             text.size(), ImGuiInputTextFlags_CharsDecimal);
            if (!enabled) ImGui::EndDisabled();
        };
        field("Quantity", draft.change_qty, draft.qty);
        field("Notional", draft.change_notional, draft.notional);
        field("Limit price", draft.change_limit_price, draft.limit_price);
        field("Stop price", draft.change_stop_price, draft.stop_price);
        field("Trail", draft.change_trail, draft.trail);
        ImGui::Checkbox("Time in force", &draft.change_time_in_force);
        ImGui::SameLine();
        if (!draft.change_time_in_force) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::Combo(
            "##time-in-force", &draft.time_in_force_index,
            [](void*, int index) {
                return kTimeInForceChoices[static_cast<std::size_t>(index)].first;
            }, nullptr, static_cast<int>(kTimeInForceChoices.size()));
        if (!draft.change_time_in_force) ImGui::EndDisabled();
        if (!draft.error.empty())
            ImGui::TextColored({.95f, .32f, .32f, 1.0f}, "%s",
                               draft.error.c_str());
        ImGui::Separator();
        if (ImGui::Button("Replace Order", {120.0f, 0.0f})) {
            core::ReplaceOrderRequest replacement;
            std::string error;
            const auto add = [&](bool enabled, const std::array<char, 64>& text,
                                 std::string_view label,
                                 std::optional<core::Decimal>& target) {
                if (!enabled) return true;
                const auto parsed = ParseReplacementDecimal(text, label, error);
                if (!parsed) return false;
                target = *parsed;
                return true;
            };
            const bool valid =
                add(draft.change_qty, draft.qty, "Quantity", replacement.qty) &&
                add(draft.change_notional, draft.notional, "Notional", replacement.notional) &&
                add(draft.change_limit_price, draft.limit_price, "Limit price", replacement.limit_price) &&
                add(draft.change_stop_price, draft.stop_price, "Stop price", replacement.stop_price) &&
                add(draft.change_trail, draft.trail, "Trail", replacement.trail);
            if (valid && draft.change_time_in_force)
                replacement.time_in_force = kTimeInForceChoices[
                    static_cast<std::size_t>(draft.time_in_force_index)].second;
            if (!valid) {
                draft.error = std::move(error);
            } else if (!replacement.qty && !replacement.notional &&
                       !replacement.limit_price && !replacement.stop_price &&
                       !replacement.trail && !replacement.time_in_force) {
                draft.error = "Select at least one field to change.";
            } else {
                action_request_ = ActionRequest{
                    .kind = ActionRequest::Kind::Replace,
                    .order_id = std::move(draft.order_id),
                    .replacement = std::move(replacement),
                };
                replace_draft_.reset();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100.0f, 0.0f})) {
            replace_draft_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    workspace.EndWindow(window);
    ImGui::PopStyleVar();
}

std::optional<OrdersWindowRenderer::ActionRequest>
OrdersWindowRenderer::ConsumeActionRequest() {
    return std::exchange(action_request_, std::nullopt);
}

void OrdersWindowRenderer::SetSubmissionResult(
    std::future<core::OrderCommandResult> result) {
    pending_result_ = std::move(result);
    result_.reset();
}

bool OrdersWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

}  // namespace tradebox::gui
