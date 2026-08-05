#include "tradebox/workstation/profile_codec.h"

#include "tradebox/workstation/validation.h"

#include <toml++/toml.hpp>

#include <iomanip>
#include <sstream>

namespace tradebox::workstation {
namespace {

std::string Quote(std::string_view value) {
    std::string output{"\""};
    for (const char character : value) {
        switch (character) {
            case '\\': output += "\\\\"; break;
            case '\"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output += character; break;
        }
    }
    output += '"';
    return output;
}

std::string Key(std::string_view value) { return Quote(value); }

template <typename T>
T ValueOr(const toml::table& table, std::string_view key, T fallback) {
    return table[key].value_or(std::move(fallback));
}

LogicalRect ReadRect(const toml::table& table, LogicalRect fallback) {
    fallback.x = ValueOr(table, "x", fallback.x);
    fallback.y = ValueOr(table, "y", fallback.y);
    fallback.width = ValueOr(table, "width", fallback.width);
    fallback.height = ValueOr(table, "height", fallback.height);
    return fallback;
}

void WriteRect(std::ostringstream& output, const LogicalRect& bounds) {
    output << "x = " << bounds.x << '\n';
    output << "y = " << bounds.y << '\n';
    output << "width = " << bounds.width << '\n';
    output << "height = " << bounds.height << '\n';
}

std::vector<std::string> ReadStringArray(const toml::table& table,
                                         std::string_view key) {
    std::vector<std::string> values;
    const toml::array* array = table[key].as_array();
    if (array == nullptr) return values;
    for (const toml::node& value : *array) {
        if (const auto text = value.value<std::string>()) values.push_back(*text);
    }
    return values;
}

void WriteStringArray(std::ostringstream& output,
                      const std::vector<std::string>& values) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output << ", ";
        output << Quote(values[index]);
    }
    output << ']';
}

}  // namespace

std::string EncodeProfile(const WorkstationState& state) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(3);
    output << "# Trade Box workstation profile. Credentials and market data are excluded.\n\n";
    output << "[profile]\n";
    output << "schema_version = " << state.profile.schema_version << '\n';
    output << "id = " << Quote(state.profile.id) << '\n';
    output << "name = " << Quote(state.profile.name) << "\n\n";

    output << "[application]\n";
    output << "ui_scale = " << state.application.ui_scale << '\n';
    output << "window_snap_pixels = " << state.application.window_snap_pixels << '\n';
    output << "vsync_requested = " << (state.application.vsync_requested ? "true" : "false") << '\n';
    output << "maximum_frame_rate = " << state.application.maximum_frame_rate << "\n\n";

    output << "[native_window]\n";
    WriteRect(output, state.native_window.bounds);
    output << "display_id = " << Quote(state.native_window.display_id) << '\n';
    output << "maximized = " << (state.native_window.maximized ? "true" : "false") << "\n\n";

    output << "[account_context]\n";
    output << "credential_slot = " << Quote(state.account_context.credential_slot) << '\n';
    output << "account_id = " << Quote(state.account_context.account_id) << '\n';
    output << "account_alias = " << Quote(state.account_context.account_alias) << '\n';
    output << "paper = " << (state.account_context.paper ? "true" : "false") << '\n';
    output << "auto_connect = " << (state.account_context.auto_connect ? "true" : "false") << "\n\n";

    output << "[workspace]\n";
    output << "selected_symbol = " << Quote(state.workspace.selected_symbol) << '\n';
    output << "watchlist = ";
    WriteStringArray(output, state.workspace.watchlist);
    output << '\n';
    output << "show_active_orders = " << (state.workspace.show_active_orders ? "true" : "false") << '\n';
    output << "show_filled_orders = " << (state.workspace.show_filled_orders ? "true" : "false") << '\n';
    output << "order_management_symbol = " << Quote(state.workspace.order_management_symbol) << '\n';
    output << "time_sales_symbol = " << Quote(state.workspace.time_sales_symbol) << '\n';
    output << "quick_long_buying_power_percent = " << state.workspace.quick_long_buying_power_percent << '\n';
    output << "quick_short_buying_power_percent = " << state.workspace.quick_short_buying_power_percent << "\n\n";

    for (const auto& [symbol, draft] : state.workspace.bracket_drafts) {
        output << "[bracket_drafts." << Key(symbol) << "]\n";
        output << "target_percent = " << draft.target_percent << '\n';
        output << "stop_percent = " << draft.stop_percent << '\n';
        output << "gtc = " << (draft.gtc ? "true" : "false") << '\n';
        output << "short_entry = " << (draft.short_entry ? "true" : "false") << "\n\n";
    }

    for (const auto& [id, window] : state.workspace.windows) {
        output << "[windows." << Key(id) << "]\n";
        output << "id = " << Quote(window.id) << '\n';
        output << "kind = " << Quote(window.kind) << '\n';
        output << "title = " << Quote(window.title) << '\n';
        output << "open = " << (window.open ? "true" : "false") << '\n';
        WriteRect(output, window.bounds);
        output << "display_id = " << Quote(window.display_id) << '\n';
        output << "selected_tab = " << Quote(window.selected_tab) << "\n\n";
        for (const auto& [table_id, table] : window.tables) {
            output << "[windows." << Key(id) << ".tables." << Key(table_id) << "]\n";
            for (const ColumnState& column : table.columns) {
                output << "[[windows." << Key(id) << ".tables." << Key(table_id)
                       << ".columns]]\n";
                output << "id = " << Quote(column.id) << '\n';
                output << "order = " << column.order << '\n';
                output << "width = " << column.width << '\n';
                output << "visible = " << (column.visible ? "true" : "false") << '\n';
                output << "sort_direction = " << Quote(column.sort_direction) << "\n\n";
            }
        }
    }

    for (const ChartDocumentState& chart : state.workspace.charts) {
        output << "[[charts]]\n";
        output << "id = " << Quote(chart.id) << '\n';
        output << "instrument_id = " << Quote(chart.instrument_id) << '\n';
        output << "symbol = " << Quote(chart.symbol) << '\n';
        output << "timeframe = " << Quote(chart.timeframe) << '\n';
        output << "visible_bars = " << chart.visible_bars << '\n';
        output << "show_volume = " << (chart.show_volume ? "true" : "false") << '\n';
        output << "show_close_line = " << (chart.show_close_line ? "true" : "false") << '\n';
        output << "show_crosshair = " << (chart.show_crosshair ? "true" : "false") << "\n\n";
    }

    for (const OrderTicketState& ticket : state.workspace.order_tickets) {
        output << "[[order_tickets]]\n";
        output << "id = " << Quote(ticket.id) << '\n';
        output << "open = " << (ticket.open ? "true" : "false") << '\n';
        output << "name = " << Quote(ticket.name) << '\n';
        output << "symbol = " << Quote(ticket.symbol) << '\n';
        output << "side = " << Quote(ticket.side) << '\n';
        output << "amount = " << Quote(ticket.amount) << '\n';
        output << "amount_is_notional = " << (ticket.amount_is_notional ? "true" : "false") << '\n';
        output << "type = " << Quote(ticket.type) << '\n';
        output << "limit_price = " << Quote(ticket.limit_price) << '\n';
        output << "stop_price = " << Quote(ticket.stop_price) << '\n';
        output << "time_in_force = " << Quote(ticket.time_in_force) << '\n';
        output << "extended_hours = " << (ticket.extended_hours ? "true" : "false") << '\n';
        output << "credential_slot = " << Quote(ticket.credential_slot) << '\n';
        output << "account_id = " << Quote(ticket.account_id) << '\n';
        output << "paper = " << (ticket.paper ? "true" : "false") << "\n\n";
    }
    return output.str();
}

std::expected<WorkstationState, std::string> DecodeProfile(std::string_view source) {
    try {
        const toml::table root = toml::parse(source);
        WorkstationState state = WorkstationState::Defaults();
        if (const toml::table* profile = root["profile"].as_table()) {
            state.profile.schema_version = ValueOr(*profile, "schema_version", 0);
            state.profile.id = ValueOr(*profile, "id", std::string{});
            state.profile.name = ValueOr(*profile, "name", state.profile.name);
        }
        if (const toml::table* application = root["application"].as_table()) {
            state.application.ui_scale = ValueOr(*application, "ui_scale", state.application.ui_scale);
            state.application.window_snap_pixels = ValueOr(*application, "window_snap_pixels", state.application.window_snap_pixels);
            state.application.vsync_requested = ValueOr(*application, "vsync_requested", state.application.vsync_requested);
            state.application.maximum_frame_rate = ValueOr(*application, "maximum_frame_rate", state.application.maximum_frame_rate);
        }
        if (const toml::table* native = root["native_window"].as_table()) {
            state.native_window.bounds = ReadRect(*native, state.native_window.bounds);
            state.native_window.display_id = ValueOr(*native, "display_id", std::string{});
            state.native_window.maximized = ValueOr(*native, "maximized", false);
        }
        if (const toml::table* account = root["account_context"].as_table()) {
            state.account_context.credential_slot = ValueOr(*account, "credential_slot", std::string{});
            state.account_context.account_id = ValueOr(*account, "account_id", std::string{});
            state.account_context.account_alias = ValueOr(*account, "account_alias", std::string{});
            state.account_context.paper = ValueOr(*account, "paper", true);
            state.account_context.auto_connect = ValueOr(*account, "auto_connect", false);
        }
        if (const toml::table* workspace = root["workspace"].as_table()) {
            state.workspace.selected_symbol = ValueOr(*workspace, "selected_symbol", state.workspace.selected_symbol);
            state.workspace.watchlist = ReadStringArray(*workspace, "watchlist");
            state.workspace.show_active_orders = ValueOr(*workspace, "show_active_orders", true);
            state.workspace.show_filled_orders = ValueOr(*workspace, "show_filled_orders", true);
            state.workspace.order_management_symbol = ValueOr(*workspace, "order_management_symbol", std::string{});
            state.workspace.time_sales_symbol = ValueOr(*workspace, "time_sales_symbol", state.workspace.time_sales_symbol);
            state.workspace.quick_long_buying_power_percent = ValueOr(*workspace, "quick_long_buying_power_percent", 100.0f);
            state.workspace.quick_short_buying_power_percent = ValueOr(*workspace, "quick_short_buying_power_percent", 80.0f);
        }
        state.workspace.bracket_drafts.clear();
        if (const toml::table* drafts = root["bracket_drafts"].as_table()) {
            for (const auto& [key, node] : *drafts) {
                const toml::table* draft_table = node.as_table();
                if (draft_table == nullptr) continue;
                BracketDraftState draft;
                draft.target_percent = ValueOr(*draft_table, "target_percent", draft.target_percent);
                draft.stop_percent = ValueOr(*draft_table, "stop_percent", draft.stop_percent);
                draft.gtc = ValueOr(*draft_table, "gtc", draft.gtc);
                draft.short_entry = ValueOr(*draft_table, "short_entry", draft.short_entry);
                state.workspace.bracket_drafts.emplace(std::string(key.str()), draft);
            }
        }
        state.workspace.windows.clear();
        if (const toml::table* windows = root["windows"].as_table()) {
            for (const auto& [key, node] : *windows) {
                const toml::table* window_table = node.as_table();
                if (window_table == nullptr) continue;
                const std::string id(key.str());
                WindowInstanceState window;
                window.id = ValueOr(*window_table, "id", id);
                window.kind = ValueOr(*window_table, "kind", std::string{"tool"});
                window.title = ValueOr(*window_table, "title", id);
                window.open = ValueOr(*window_table, "open", true);
                window.bounds = ReadRect(*window_table, {24, 24, 420, 280});
                window.display_id = ValueOr(*window_table, "display_id", std::string{});
                window.selected_tab = ValueOr(*window_table, "selected_tab", std::string{});
                if (const toml::table* tables = (*window_table)["tables"].as_table()) {
                    for (const auto& [table_key, table_node] : *tables) {
                        const toml::table* table_value = table_node.as_table();
                        if (table_value == nullptr) continue;
                        PersistentTableState table;
                        if (const toml::array* columns = (*table_value)["columns"].as_array()) {
                            for (const toml::node& column_node : *columns) {
                                const toml::table* column_value = column_node.as_table();
                                if (column_value == nullptr) continue;
                                table.columns.push_back({
                                    .id = ValueOr(*column_value, "id", std::string{}),
                                    .order = ValueOr(*column_value, "order", 0),
                                    .width = ValueOr(*column_value, "width", 0.0f),
                                    .visible = ValueOr(*column_value, "visible", true),
                                    .sort_direction = ValueOr(*column_value, "sort_direction", std::string{}),
                                });
                            }
                        }
                        window.tables.emplace(std::string(table_key.str()), std::move(table));
                    }
                }
                state.workspace.windows.emplace(id, std::move(window));
            }
        }
        state.workspace.charts.clear();
        if (const toml::array* charts = root["charts"].as_array()) {
            for (const toml::node& node : *charts) {
                const toml::table* chart = node.as_table();
                if (chart == nullptr) continue;
                state.workspace.charts.push_back({
                    .id = ValueOr(*chart, "id", std::string{}),
                    .instrument_id = ValueOr(*chart, "instrument_id", std::string{}),
                    .symbol = ValueOr(*chart, "symbol", std::string{}),
                    .timeframe = ValueOr(*chart, "timeframe", std::string{"1Min"}),
                    .visible_bars = ValueOr(*chart, "visible_bars", 120),
                    .show_volume = ValueOr(*chart, "show_volume", true),
                    .show_close_line = ValueOr(*chart, "show_close_line", false),
                    .show_crosshair = ValueOr(*chart, "show_crosshair", true),
                });
            }
        }
        state.workspace.order_tickets.clear();
        if (const toml::array* tickets = root["order_tickets"].as_array()) {
            for (const toml::node& node : *tickets) {
                const toml::table* ticket = node.as_table();
                if (ticket == nullptr) continue;
                state.workspace.order_tickets.push_back({
                    .id = ValueOr(*ticket, "id", std::string{}),
                    .open = ValueOr(*ticket, "open", true),
                    .name = ValueOr(*ticket, "name", std::string{"Untitled order"}),
                    .symbol = ValueOr(*ticket, "symbol", std::string{}),
                    .side = ValueOr(*ticket, "side", std::string{"buy"}),
                    .amount = ValueOr(*ticket, "amount", std::string{"1"}),
                    .amount_is_notional = ValueOr(*ticket, "amount_is_notional", false),
                    .type = ValueOr(*ticket, "type", std::string{"market"}),
                    .limit_price = ValueOr(*ticket, "limit_price", std::string{}),
                    .stop_price = ValueOr(*ticket, "stop_price", std::string{}),
                    .time_in_force = ValueOr(*ticket, "time_in_force", std::string{"day"}),
                    .extended_hours = ValueOr(*ticket, "extended_hours", false),
                    .credential_slot = ValueOr(*ticket, "credential_slot", std::string{}),
                    .account_id = ValueOr(*ticket, "account_id", std::string{}),
                    .paper = ValueOr(*ticket, "paper", true),
                });
            }
        }
        std::string validation_error;
        if (!ValidateAndNormalize(state, validation_error))
            return std::unexpected(validation_error);
        return state;
    } catch (const toml::parse_error& error) {
        return std::unexpected(std::string("Invalid workstation profile: ") +
                               std::string(error.description()));
    } catch (const std::exception& error) {
        return std::unexpected(std::string("Could not decode workstation profile: ") + error.what());
    }
}

}  // namespace tradebox::workstation

