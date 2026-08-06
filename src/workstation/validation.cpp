#include "tradebox/workstation/validation.h"
#include "tradebox/workstation/instrument_links.h"
#include "tradebox/workstation/asset_preferences.h"

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

bool ValidateIndicators(std::vector<ChartIndicatorState>& indicators,
                        std::string_view owner, std::string& error) {
    std::set<std::string, std::less<>> ids;
    for (ChartIndicatorState& indicator : indicators) {
        if (indicator.definition.id.empty() ||
            !ids.insert(indicator.definition.id).second) {
            error = std::string(owner) +
                    " contains an invalid or duplicate indicator ID";
            return false;
        }
        std::visit(
            [](auto& calculation) {
                calculation.period =
                    std::clamp(calculation.period, 1U, 10'000U);
            },
            indicator.definition.calculation);
        indicator.line_width =
            std::clamp(indicator.line_width, 0.5f, 10.0f);
    }
    std::vector<core::IndicatorDefinition> definitions;
    definitions.reserve(indicators.size());
    for (const ChartIndicatorState& indicator : indicators)
        definitions.push_back(indicator.definition);
    const auto evaluated = core::EvaluateIndicators(definitions, {});
    if (!evaluated) {
        error = std::string(owner) + ": " + evaluated.error().message;
        return false;
    }
    return true;
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

    std::set<std::string, std::less<>> link_group_ids;
    std::set<InstrumentLinkColor> link_group_colors;
    for (InstrumentLinkGroupState& group :
         state.workspace.instrument_link_groups) {
        const auto color_index = static_cast<std::size_t>(group.color);
        if (color_index >= kInstrumentLinkGroupCount || group.id.empty() ||
            group.id != InstrumentLinkGroupId(group.color) ||
            !link_group_ids.insert(group.id).second ||
            !link_group_colors.insert(group.color).second) {
            error = "Instrument link groups contain invalid or duplicate identity";
            return false;
        }
        if (group.name.empty())
            group.name = std::string(InstrumentLinkColorName(group.color));
        if (group.selected_instrument &&
            (group.selected_instrument->instrument_id.empty() ||
             group.selected_instrument->symbol.empty())) {
            error = "Instrument link selection requires stable identity and symbol";
            return false;
        }
    }

    std::set<std::string, std::less<>> watchlist;
    std::vector<std::string> unique_watchlist;
    for (const std::string& symbol : state.workspace.watchlist) {
        if (!symbol.empty() && watchlist.insert(symbol).second)
            unique_watchlist.push_back(symbol);
    }
    state.workspace.watchlist = std::move(unique_watchlist);
    if (state.workspace.selected_symbol.empty() && !state.workspace.watchlist.empty())
        state.workspace.selected_symbol = state.workspace.watchlist.front();

    std::set<std::string, std::less<>> selected_assets;
    std::vector<std::string> normalized_selection_history;
    normalized_selection_history.reserve(
        std::min(state.workspace.asset_selection_history.size(),
                 kAssetSelectionHistoryLimit));
    for (const std::string& instrument_id :
         state.workspace.asset_selection_history) {
        if (!instrument_id.empty() && selected_assets.insert(instrument_id).second)
            normalized_selection_history.push_back(instrument_id);
        if (normalized_selection_history.size() ==
            kAssetSelectionHistoryLimit)
            break;
    }
    state.workspace.asset_selection_history =
        std::move(normalized_selection_history);

    for (auto& [id, window] : state.workspace.windows) {
        if (id.empty() || window.id != id || window.kind.empty()) {
            error = "Window profile contains an invalid stable ID";
            return false;
        }
        if (!window.instrument_link_group_id.empty() &&
            !link_group_ids.contains(window.instrument_link_group_id)) {
            error = "Window profile references an unknown instrument link group";
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

    std::set<std::string, std::less<>> watch_list_ids;
    std::set<std::string, std::less<>> watch_list_row_ids;
    for (WatchListDocumentState& watch_list : state.workspace.watch_lists) {
        if (watch_list.id.empty() || watch_list.name.empty() ||
            !watch_list_ids.insert(watch_list.id).second) {
            error = "Watch-list profile contains an invalid or duplicate document ID";
            return false;
        }
        const auto window = state.workspace.windows.find(watch_list.id);
        if (window == state.workspace.windows.end()) {
            state.workspace.windows.emplace(
                watch_list.id,
                WindowInstanceState{
                    .id = watch_list.id,
                    .kind = "watch-list",
                    .title = watch_list.name,
                    .open = true,
                    .bounds = {72.0f, 72.0f, 720.0f, 480.0f},
                });
        } else if (window->second.kind != "watch-list") {
            error = "Watch-list document ID is owned by a non-watch-list window";
            return false;
        }
        for (WatchListRowState& row : watch_list.rows) {
            if (row.id.empty() || !watch_list_row_ids.insert(row.id).second ||
                row.instrument_id.empty() != row.symbol.empty()) {
                error = "Watch-list profile contains an invalid row";
                return false;
            }
            if (!row.symbol.empty()) row.ticker_input = row.symbol;
        }
    }

    state.workspace.chart_defaults.visible_bars = std::clamp(
        state.workspace.chart_defaults.visible_bars, 30, 2000);
    if (state.workspace.chart_defaults.timeframe.empty())
        state.workspace.chart_defaults.timeframe = "1Min";
    if (!ValidateIndicators(
            state.workspace.chart_defaults.indicators,
            "Chart defaults", error))
        return false;

    std::set<std::string, std::less<>> chart_ids;
    for (ChartDocumentState& chart : state.workspace.charts) {
        if (chart.id.empty() || !chart_ids.insert(chart.id).second ||
            chart.instrument_id.empty() != chart.symbol.empty()) {
            error = "Chart profile contains an invalid or duplicate document ID";
            return false;
        }
        chart.visible_bars = std::clamp(chart.visible_bars, 30, 2000);
        if (chart.timeframe.empty()) chart.timeframe = "1Min";
        if (!chart.symbol.empty()) chart.ticker_input = chart.symbol;
        if (!ValidateIndicators(chart.indicators, "Chart document", error))
            return false;
        const auto window = state.workspace.windows.find(chart.id);
        if (window == state.workspace.windows.end()) {
            state.workspace.windows.emplace(
                chart.id,
                WindowInstanceState{
                    .id = chart.id,
                    .kind = "chart",
                    .title = chart.symbol.empty()
                                 ? "Chart"
                                 : chart.symbol + " · " + chart.timeframe,
                    .open = true,
                    .bounds = {48.0f, 48.0f, 960.0f, 640.0f},
                });
        } else if (window->second.kind != "chart") {
            error = "Chart document ID is owned by a non-chart window";
            return false;
        }
    }

    std::set<std::string, std::less<>> suite_ids;
    std::set<std::string, std::less<>> suite_names;
    for (IndicatorSuiteState& suite : state.workspace.indicator_suites) {
        if (suite.id.empty() || suite.name.empty() ||
            !suite_ids.insert(suite.id).second ||
            !suite_names.insert(suite.name).second) {
            error = "Indicator-suite profile contains invalid or duplicate identity";
            return false;
        }
        if (!ValidateIndicators(suite.indicators, "Indicator suite", error))
            return false;
    }

    std::set<std::string, std::less<>> drawing_ids;
    for (ChartDrawingState& drawing : state.workspace.chart_drawings) {
        if (drawing.id.empty() || drawing.instrument_id.empty() ||
            !drawing_ids.insert(drawing.id).second) {
            error = "Chart drawing contains invalid or duplicate identity";
            return false;
        }
        drawing.line_width = std::clamp(drawing.line_width, 0.5f, 10.0f);
        const bool needs_second =
            drawing.kind == ChartDrawingKind::TrendLine ||
            drawing.kind == ChartDrawingKind::Ray ||
            drawing.kind == ChartDrawingKind::Rectangle;
        if (needs_second && !drawing.second) {
            error = "Chart drawing is missing its second anchor";
            return false;
        }
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
