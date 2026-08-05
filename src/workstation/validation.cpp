#include "tradebox/workstation/validation.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace tradebox::workstation {
namespace {

bool Finite(float value) { return std::isfinite(value); }

void NormalizeBounds(LogicalRect& bounds, float minimum_width,
                     float minimum_height) {
    if (!Finite(bounds.x)) bounds.x = 0.0f;
    if (!Finite(bounds.y)) bounds.y = 0.0f;
    if (!Finite(bounds.width)) bounds.width = minimum_width;
    if (!Finite(bounds.height)) bounds.height = minimum_height;
    bounds.x = std::clamp(bounds.x, -100000.0f, 100000.0f);
    bounds.y = std::clamp(bounds.y, -100000.0f, 100000.0f);
    bounds.width = std::clamp(bounds.width, minimum_width, 100000.0f);
    bounds.height = std::clamp(bounds.height, minimum_height, 100000.0f);
}

}  // namespace

bool ValidateAndNormalize(WorkstationState& state, std::string& error) {
    if (state.profile.schema_version != kCurrentSchemaVersion) {
        error = "Unsupported workstation profile schema version";
        return false;
    }
    if (state.profile.id.empty()) {
        error = "Workstation profile has no stable ID";
        return false;
    }
    if (state.profile.name.empty()) state.profile.name = "Unnamed profile";
    state.application.ui_scale = std::clamp(state.application.ui_scale, 0.70f, 2.0f);
    state.application.window_snap_pixels =
        std::clamp(state.application.window_snap_pixels, 1, 1000);
    state.application.maximum_frame_rate =
        std::clamp(state.application.maximum_frame_rate, 0, 10000);
    NormalizeBounds(state.native_window.bounds, 640.0f, 480.0f);

    std::set<std::string, std::less<>> watchlist;
    std::vector<std::string> unique_watchlist;
    for (const std::string& symbol : state.workspace.watchlist) {
        if (!symbol.empty() && watchlist.insert(symbol).second)
            unique_watchlist.push_back(symbol);
    }
    state.workspace.watchlist = std::move(unique_watchlist);
    if (state.workspace.selected_symbol.empty() && !state.workspace.watchlist.empty())
        state.workspace.selected_symbol = state.workspace.watchlist.front();

    for (auto& [id, window] : state.workspace.windows) {
        if (id.empty() || window.id != id) {
            error = "Window profile contains an invalid stable ID";
            return false;
        }
        NormalizeBounds(window.bounds, 160.0f, 100.0f);
        for (auto& [table_id, table] : window.tables) {
            if (table_id.empty()) {
                error = "Window profile contains an unnamed table";
                return false;
            }
            for (ColumnState& column : table.columns) {
                if (column.id.empty()) {
                    error = "Table profile contains an unnamed column";
                    return false;
                }
                column.width = std::clamp(column.width, 0.0f, 100000.0f);
            }
        }
    }

    std::set<std::string, std::less<>> chart_ids;
    for (ChartDocumentState& chart : state.workspace.charts) {
        if (chart.id.empty() || !chart_ids.insert(chart.id).second || chart.symbol.empty()) {
            error = "Chart profile contains an invalid or duplicate document ID";
            return false;
        }
        chart.visible_bars = std::clamp(chart.visible_bars, 30, 2000);
        if (chart.timeframe.empty()) chart.timeframe = "1Min";
    }

    std::set<std::string, std::less<>> ticket_ids;
    for (OrderTicketState& ticket : state.workspace.order_tickets) {
        if (ticket.id.empty() || !ticket_ids.insert(ticket.id).second) {
            error = "Order-ticket profile contains an invalid or duplicate document ID";
            return false;
        }
        if (ticket.name.empty()) ticket.name = "Untitled order";
        if (ticket.symbol.empty()) ticket.symbol = state.workspace.selected_symbol;
    }
    for (auto& [symbol, draft] : state.workspace.bracket_drafts) {
        if (symbol.empty()) {
            error = "Bracket draft profile contains an empty symbol";
            return false;
        }
        draft.target_percent = std::clamp(draft.target_percent, 0.01f, 100.0f);
        draft.stop_percent = std::clamp(draft.stop_percent, 0.01f, 100.0f);
    }
    return true;
}

}  // namespace tradebox::workstation

