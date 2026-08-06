#include "watch_list_window.h"

#include "tradebox/workstation/asset_preferences.h"
#include "tradebox/workstation/watch_list_columns.h"
#include "tradebox/workstation/watch_list_documents.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace tradebox::gui {
namespace {

const application::UiAssetSearchResult* MatchesFor(
    const application::ApplicationUiSnapshot& snapshot,
    std::string_view query) {
    const auto found = std::ranges::find(
        snapshot.asset_search_results, query,
        &application::UiAssetSearchResult::query);
    return found == snapshot.asset_search_results.end() ? nullptr : &*found;
}

const application::UiWatchListSnapshot* SnapshotFor(
    const application::ApplicationUiSnapshot& snapshot,
    std::string_view document_id) {
    const auto found = std::ranges::find(
        snapshot.watch_lists, document_id,
        &application::UiWatchListSnapshot::document_id);
    return found == snapshot.watch_lists.end() ? nullptr : &*found;
}

const application::UiWatchListRowSnapshot* RowSnapshotFor(
    const application::UiWatchListSnapshot* snapshot,
    std::string_view row_id) {
    if (snapshot == nullptr) return nullptr;
    const auto found = std::ranges::find(
        snapshot->rows, row_id,
        &application::UiWatchListRowSnapshot::row_id);
    return found == snapshot->rows.end() ? nullptr : &*found;
}

std::string TableColumnLabel(
    const workstation::WatchListColumnDefinition& definition) {
    return std::string(definition.label) + "###" +
           std::string(definition.id);
}

std::string ColumnIdFromTableName(const char* name) {
    if (name == nullptr) return {};
    const std::string_view value(name);
    const std::size_t separator = value.find("###");
    return std::string(value.substr(
        separator == std::string_view::npos ? 0 : separator + 3));
}

std::string SignedDecimal(const core::Decimal& value) {
    std::string text = value.ToString();
    if (!value.IsZero() && text.front() != '-') text.insert(text.begin(), '+');
    return text;
}

std::vector<workstation::ColumnState*> OrderedColumns(
    workstation::PersistentTableState& table) {
    std::vector<workstation::ColumnState*> result;
    result.reserve(table.columns.size());
    for (workstation::ColumnState& column : table.columns)
        result.push_back(&column);
    std::ranges::stable_sort(result, [](const auto* left, const auto* right) {
        if (left->order != right->order) return left->order < right->order;
        return left->id < right->id;
    });
    return result;
}

std::optional<core::Decimal> NumericValueFor(
    const workstation::WatchListRowState& row,
    std::string_view column_id,
    const application::UiWatchListSnapshot* snapshot) {
    const auto* row_snapshot = RowSnapshotFor(snapshot, row.id);
    if (row_snapshot == nullptr) return std::nullopt;
    if (column_id == "current_price") return row_snapshot->current_price;
    if (column_id == "change_from_open")
        return row_snapshot->change_from_open;
    return std::nullopt;
}

void SortRows(
    std::vector<workstation::WatchListRowState>& rows,
    std::string_view column_id, ImGuiSortDirection direction,
    const application::UiWatchListSnapshot* snapshot) {
    const bool descending = direction == ImGuiSortDirection_Descending;
    std::ranges::stable_sort(rows, [&](const auto& left, const auto& right) {
        if (column_id == "symbol") {
            if (left.symbol != right.symbol)
                return descending ? left.symbol > right.symbol
                                   : left.symbol < right.symbol;
            return left.id < right.id;
        }
        const auto left_value = NumericValueFor(left, column_id, snapshot);
        const auto right_value = NumericValueFor(right, column_id, snapshot);
        if (left_value && right_value && *left_value != *right_value) {
            return descending ? *left_value > *right_value
                              : *left_value < *right_value;
        }
        if (left_value.has_value() != right_value.has_value())
            return left_value.has_value();
        return left.id < right.id;
    });
}

}  // namespace

void WatchListWindowRenderer::AppendSnapshotQuery(
    workstation::WorkspaceState& state, application::UiSnapshotQuery& query) {
    queries_.clear();
    for (const workstation::WatchListDocumentState& document :
         state.watch_lists) {
        const auto window = state.windows.find(document.id);
        if (window == state.windows.end() || !window->second.open) continue;
        const auto table = window->second.tables.find(
            std::string(workstation::kWatchListTableId));
        const bool needs_change_from_open =
            table != window->second.tables.end() &&
            std::ranges::any_of(table->second.columns, [](const auto& column) {
                return column.visible && column.id == "change_from_open";
            });

        application::UiWatchListQuery watch_query{
            .document_id = document.id,
            .needs_change_from_open = needs_change_from_open,
        };
        for (const workstation::WatchListRowState& row : document.rows) {
            if (!row.instrument_id.empty() && !row.symbol.empty()) {
                query.market_symbols.push_back(row.symbol);
                watch_query.rows.push_back({
                    .row_id = row.id,
                    .instrument_id = row.instrument_id,
                    .symbol = row.symbol,
                });
            } else if (!row.ticker_input.empty()) {
                query.asset_searches.push_back(row.ticker_input);
            }
        }
        queries_.emplace(document.id, watch_query);
        query.watch_lists.push_back(std::move(watch_query));
    }
    query.asset_preferred_instrument_ids = state.asset_selection_history;
    if (!query.asset_searches.empty())
        query.asset_limit = std::max<std::size_t>(query.asset_limit, 8U);
}

void WatchListWindowRenderer::RequestMissingHistory(
    application::TradingApplication& application,
    const application::ApplicationUiSnapshot& snapshot) {
    for (const application::UiWatchListSnapshot& watch_list :
         snapshot.watch_lists) {
        if (watch_list.daily_range.start_ns >= watch_list.daily_range.end_ns)
            continue;
        const auto query = queries_.find(watch_list.document_id);
        if (query == queries_.end()) continue;
        for (const auto& row : watch_list.rows) {
            if (!row.history_missing) continue;
            const auto row_query = std::ranges::find(
                query->second.rows, row.row_id,
                &application::UiWatchListRowQuery::row_id);
            if (row_query == query->second.rows.end()) continue;
            const auto requested = requested_history_.find(row.row_id);
            if (requested != requested_history_.end() &&
                requested->second == watch_list.daily_range)
                continue;
            application.RequestMarketHistory(
                row_query->symbol, "1Day", watch_list.daily_range);
            requested_history_[row.row_id] = watch_list.daily_range;
        }
    }
}

void WatchListWindowRenderer::Draw(
    ui::Workspace& workspace, workstation::WorkspaceState& state,
    const application::ApplicationUiSnapshot& snapshot) {
    for (workstation::WatchListDocumentState& document : state.watch_lists) {
        const auto window_state = state.windows.find(document.id);
        if (window_state == state.windows.end() || !window_state->second.open)
            continue;
        auto table = window_state->second.tables.find(
            std::string(workstation::kWatchListTableId));
        if (table == window_state->second.tables.end()) continue;
        const auto columns = OrderedColumns(table->second);
        if (columns.empty()) continue;

        ui::WorkspaceWindow window{
            .title = document.name,
            .id = document.id,
            .default_offset = {72.0f, 72.0f},
            .default_size = {720.0f, 480.0f},
            .open = true,
        };
        workspace.ConstrainNextWindowSize({520.0f, 220.0f});
        if (!workspace.BeginWindow(window)) {
            workspace.EndWindow(window);
            continue;
        }

        const auto* watch_snapshot = SnapshotFor(snapshot, document.id);
        ImGui::PushID(document.id.c_str());
        const std::string table_id = "##watch_list_table";
        const ImGuiTableFlags table_flags =
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_ScrollX | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable(table_id.c_str(),
                              static_cast<int>(columns.size()) + 1,
                              table_flags)) {
            for (const workstation::ColumnState* column : columns) {
                const auto* definition =
                    workstation::FindWatchListColumn(column->id);
                const std::string label =
                    definition == nullptr
                        ? column->id + "###" + column->id
                        : TableColumnLabel(*definition);
                ImGuiTableColumnFlags column_flags =
                    ImGuiTableColumnFlags_WidthFixed;
                if (column->sort_direction == "descending")
                    column_flags |= ImGuiTableColumnFlags_DefaultSort |
                                    ImGuiTableColumnFlags_PreferSortDescending;
                else if (column->sort_direction == "ascending")
                    column_flags |= ImGuiTableColumnFlags_DefaultSort;
                ImGui::TableSetupColumn(
                    label.c_str(), column_flags,
                    column->width > 0.0f ? column->width : 140.0f);
            }
            ImGui::TableSetupColumn(
                "+###watch_list_add_column",
                ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_NoSort |
                    ImGuiTableColumnFlags_NoReorder,
                34.0f);

            std::vector<std::string> display_column_ids;
            display_column_ids.reserve(columns.size());
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (std::size_t index = 0; index < columns.size() + 1U;
                 ++index) {
                ImGui::TableNextColumn();
                const std::string id =
                    ColumnIdFromTableName(ImGui::TableGetColumnName());
                if (id == "watch_list_add_column") {
                    if (ImGui::SmallButton("+"))
                        ImGui::OpenPopup("watch_list_add_column");
                } else {
                    display_column_ids.push_back(id);
                    const auto* definition =
                        workstation::FindWatchListColumn(id);
                    ImGui::TableHeader(
                        definition == nullptr ? id.c_str()
                                               : definition->label.data());
                }
            }

            if (const ImGuiTableSortSpecs* sort_specs =
                    ImGui::TableGetSortSpecs();
                sort_specs != nullptr && sort_specs->SpecsCount > 0) {
                const auto& spec = sort_specs->Specs[0];
                if (spec.ColumnIndex >= 0 &&
                    spec.ColumnIndex < static_cast<int>(columns.size())) {
                    workstation::ColumnState& sorted_column =
                        *columns[static_cast<std::size_t>(spec.ColumnIndex)];
                    const std::string next_direction =
                        spec.SortDirection == ImGuiSortDirection_Descending
                            ? "descending"
                            : "ascending";
                    if (sorted_column.sort_direction != next_direction) {
                        sorted_column.sort_direction = next_direction;
                        persistent_changed_ = true;
                    }
                    SortRows(document.rows, sorted_column.id,
                             spec.SortDirection, watch_snapshot);
                }
            }

            for (workstation::WatchListRowState& row : document.rows) {
                ImGui::PushID(row.id.c_str());
                ImGui::TableNextRow();
                for (const std::string& column_id : display_column_ids) {
                    ImGui::TableNextColumn();
                    if (column_id == "symbol") {
                        RowInteraction& interaction = interactions_[row.id];
                        if (!interaction.initialized) {
                            const auto count = std::min(
                                row.ticker_input.size(),
                                interaction.ticker.size() - 1U);
                            std::copy_n(row.ticker_input.data(), count,
                                        interaction.ticker.data());
                            interaction.ticker[count] = '\0';
                            interaction.initialized = true;
                        }
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ImGui::InputText(
                                "##ticker", interaction.ticker.data(),
                                interaction.ticker.size(),
                                ImGuiInputTextFlags_CharsUppercase)) {
                            row.ticker_input = interaction.ticker.data();
                            row.instrument_id.clear();
                            row.symbol.clear();
                            interaction.highlighted_match = -1;
                            persistent_changed_ = true;
                        }
                        const auto* matches =
                            row.instrument_id.empty() &&
                                    !row.ticker_input.empty()
                                ? MatchesFor(snapshot, row.ticker_input)
                                : nullptr;
                        if (matches != nullptr && !matches->matches.empty()) {
                            const int last_match = static_cast<int>(
                                matches->matches.size() - 1U);
                            if (ImGui::IsItemActive() &&
                                ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                                interaction.highlighted_match = std::min(
                                    interaction.highlighted_match + 1,
                                    last_match);
                            if (ImGui::IsItemActive() &&
                                ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                                interaction.highlighted_match = std::max(
                                    interaction.highlighted_match - 1, 0);
                            const bool select_with_enter =
                                ImGui::IsItemActive() &&
                                ImGui::IsKeyPressed(ImGuiKey_Enter);
                            for (std::size_t index = 0;
                                 index < matches->matches.size(); ++index) {
                                const core::TradableAsset& asset =
                                    matches->matches[index];
                                const std::string label =
                                    asset.symbol + " - " + asset.name;
                                const bool selected =
                                    interaction.highlighted_match ==
                                    static_cast<int>(index);
                                if (selected) ImGui::SetItemDefaultFocus();
                                if (!ImGui::Selectable(label.c_str(), selected) &&
                                    !(select_with_enter && selected))
                                    continue;
                                const auto assigned =
                                    workstation::AssignWatchListRowAsset(
                                        state, document.id, row.id,
                                        asset.instrument_id, asset.symbol);
                                if (assigned) {
                                    workstation::RecordAssetSelection(
                                        state, asset.instrument_id);
                                    const auto count = std::min(
                                        asset.symbol.size(),
                                        interaction.ticker.size() - 1U);
                                    std::copy_n(asset.symbol.data(), count,
                                                interaction.ticker.data());
                                    interaction.ticker[count] = '\0';
                                    interaction.highlighted_match = -1;
                                    persistent_changed_ = true;
                                }
                            }
                        }
                    } else if (column_id == "current_price") {
                        const auto* row_snapshot =
                            RowSnapshotFor(watch_snapshot, row.id);
                        ImGui::TextUnformatted(
                            row_snapshot != nullptr &&
                                    row_snapshot->current_price
                                ? row_snapshot->current_price->ToString().c_str()
                                : "--");
                    } else if (column_id == "change_from_open") {
                        const auto* row_snapshot =
                            RowSnapshotFor(watch_snapshot, row.id);
                        const std::string value =
                            row_snapshot != nullptr &&
                                    row_snapshot->change_from_open
                                ? SignedDecimal(
                                      *row_snapshot->change_from_open)
                                : "--";
                        ImGui::TextUnformatted(value.c_str());
                    } else {
                        ImGui::TextDisabled("--");
                    }
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled(" ");
                ImGui::PopID();
            }

            ImGui::EndTable();

            std::vector<workstation::ColumnState> reordered;
            reordered.reserve(columns.size());
            for (const std::string& id : display_column_ids) {
                const auto found = std::ranges::find_if(
                    table->second.columns,
                    [&](const auto& column) { return column.id == id; });
                if (found != table->second.columns.end())
                    reordered.push_back(*found);
            }
            if (reordered.size() == table->second.columns.size()) {
                bool order_changed = false;
                for (std::size_t index = 0; index < reordered.size(); ++index)
                    order_changed = order_changed ||
                                    reordered[index].id !=
                                        table->second.columns[index].id;
                if (order_changed) persistent_changed_ = true;
                for (std::size_t index = 0; index < reordered.size(); ++index)
                    reordered[index].order = static_cast<int>(index);
                table->second.columns = std::move(reordered);
            }
        }

        if (ImGui::BeginPopup("watch_list_add_column")) {
            for (const auto& definition :
                 workstation::kWatchListColumnDefinitions) {
                if (definition.kind == workstation::WatchListColumnKind::Symbol ||
                    std::ranges::any_of(
                        table->second.columns, [&](const auto& column) {
                            return column.id == definition.id;
                        }))
                    continue;
                if (ImGui::MenuItem(definition.label.data())) {
                    if (workstation::AddWatchListColumn(
                            state, document.id, definition.kind))
                        persistent_changed_ = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        if (ImGui::Button("+ Add row")) {
            if (workstation::AddWatchListRow(state, document.id))
                persistent_changed_ = true;
        }
        ImGui::PopID();
        workspace.EndWindow(window);
    }
}

bool WatchListWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

}  // namespace tradebox::gui
