#include "watch_list_window.h"

#include "tradebox/workstation/asset_preferences.h"
#include "tradebox/workstation/stable_id.h"
#include "tradebox/workstation/watch_list_columns.h"
#include "tradebox/workstation/watch_list_documents.h"

#include "gui_controls.h"
#include "watch_list_autocomplete.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iomanip>
#include <optional>
#include <numeric>
#include <ranges>
#include <sstream>
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

constexpr std::string_view kWatchListRowPayload =
    "tradebox.watch-list-row";

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

std::string PercentageText(const core::Decimal& value) {
    std::ostringstream text;
    if (!value.IsZero() && value.ToString().front() != '-') text << '+';
    text << std::fixed << std::setprecision(2) << value.ToDisplayDouble()
         << "%";
    return text.str();
}

void DrawPercentage(const std::optional<core::Decimal>& value) {
    if (!value) {
        ImGui::TextUnformatted("--");
        return;
    }
    const ImVec4 color = value->ToString().front() == '-'
                             ? ImVec4(0.95f, 0.30f, 0.30f, 1.0f)
                             : ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
    const std::string text = PercentageText(*value);
    ImGui::TextColored(color, "%s", text.c_str());
}

std::vector<workstation::ColumnState*> OrderedColumns(
    workstation::PersistentTableState& table) {
    std::vector<workstation::ColumnState*> result;
    result.reserve(table.columns.size());
    for (workstation::ColumnState& column : table.columns)
        if (column.visible) result.push_back(&column);
    std::ranges::stable_sort(result, [](const auto* left, const auto* right) {
        if (left->order != right->order) return left->order < right->order;
        return left->id < right->id;
    });
    return result;
}

int WatchListTickerInputCallback(ImGuiInputTextCallbackData* data) {
    if (data == nullptr || data->EventFlag != ImGuiInputTextFlags_CallbackHistory ||
        data->UserData == nullptr)
        return 0;

    auto* direction = static_cast<int*>(data->UserData);
    if (data->EventKey == ImGuiKey_DownArrow)
        *direction = 1;
    else if (data->EventKey == ImGuiKey_UpArrow)
        *direction = -1;
    return 0;
}

std::optional<core::Decimal> NumericValueFor(
    const workstation::WatchListRowState& row,
    std::string_view column_id,
    const application::UiWatchListSnapshot* snapshot) {
    const auto* row_snapshot = RowSnapshotFor(snapshot, row.id);
    if (row_snapshot == nullptr) return std::nullopt;
    if (column_id == "current_price") return row_snapshot->current_price;
    if (column_id == "change_from_close")
        return row_snapshot->change_from_close;
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

void WatchListWindowRenderer::StartNewDraft(
    workstation::WorkspaceState& state) {
    draft_ = workstation::WatchListDocumentState{
        .id = std::string(workstation::kWatchListDraftId),
        .name = "Untitled Watch List",
        .rows = {{.id = workstation::NewStableId("watch-list-row")}},
    };
    if (!state.active_watch_list_id.empty()) {
        state.active_watch_list_id.clear();
        persistent_changed_ = true;
    }
    editing_name_ = false;
    focus_name_input_ = false;
    message_.clear();
    static_cast<void>(workstation::EnsureWatchListWindow(state));
    state.windows.at(std::string(workstation::kWatchListWindowId)).open = true;
}

void WatchListWindowRenderer::EnsureSession(
    workstation::WorkspaceState& state) {
    if (!state.active_watch_list_id.empty()) {
        if (workstation::FindWatchListDocument(
                state, state.active_watch_list_id) != nullptr) {
            draft_.reset();
            return;
        }
        state.active_watch_list_id.clear();
        persistent_changed_ = true;
    }
    if (!draft_) {
        const bool had_active_document = !state.active_watch_list_id.empty();
        const auto ensured = workstation::EnsureDefaultWatchList(state);
        if (!ensured) {
            message_ = ensured.error().message;
            return;
        }
        if (!had_active_document) persistent_changed_ = true;
    }
}

workstation::WatchListDocumentState*
WatchListWindowRenderer::ActiveDocument(workstation::WorkspaceState& state) {
    EnsureSession(state);
    if (!state.active_watch_list_id.empty())
        return workstation::FindWatchListDocument(
            state, state.active_watch_list_id);
    return draft_ ? &*draft_ : nullptr;
}

void WatchListWindowRenderer::SaveCurrentDraft(
    workstation::WorkspaceState& state) {
    if (!draft_) return;
    const auto saved = workstation::SaveWatchListDocument(state, *draft_);
    if (!saved) {
        message_ = saved.error().message;
        return;
    }
    draft_.reset();
    editing_name_ = false;
    focus_name_input_ = false;
    message_.clear();
    persistent_changed_ = true;
}

void WatchListWindowRenderer::OpenSavedDocument(
    workstation::WorkspaceState& state, std::string_view document_id) {
    const auto opened =
        workstation::OpenWatchListDocument(state, document_id);
    if (!opened) {
        message_ = opened.error().message;
        return;
    }
    draft_.reset();
    editing_name_ = false;
    focus_name_input_ = false;
    message_.clear();
    persistent_changed_ = true;
}

void WatchListWindowRenderer::DeleteSavedDocument(
    workstation::WorkspaceState& state, std::string_view document_id) {
    const auto deleted =
        workstation::DeleteWatchListDocument(state, document_id);
    if (!deleted) {
        message_ = deleted.error().message;
        return;
    }
    if (state.active_watch_list_id.empty()) draft_.reset();
    message_.clear();
    persistent_changed_ = true;
}

void WatchListWindowRenderer::CommitName(workstation::WorkspaceState& state) {
    const std::string next_name(name_input_.data());
    if (next_name.empty()) {
        message_ = "Watch list name cannot be empty";
        return;
    }
    if (draft_) {
        draft_->name = next_name;
        message_.clear();
    } else if (!state.active_watch_list_id.empty()) {
        const auto renamed = workstation::RenameWatchListDocument(
            state, state.active_watch_list_id, next_name);
        if (!renamed) {
            message_ = renamed.error().message;
            return;
        }
        message_.clear();
        persistent_changed_ = true;
    }
    editing_name_ = false;
    focus_name_input_ = false;
}

void WatchListWindowRenderer::AppendSnapshotQuery(
    workstation::WorkspaceState& state, application::UiSnapshotQuery& query) {
    queries_.clear();
    workstation::WatchListDocumentState* document = ActiveDocument(state);
    if (document == nullptr) return;
    if (workstation::EnsureWatchListTrailingEmptyRow(*document))
        persistent_changed_ = true;
    const auto window = state.windows.find(
        std::string(workstation::kWatchListWindowId));
    bool needs_change_from_close = false;
    if (window != state.windows.end()) {
        const auto table = window->second.tables.find(
            std::string(workstation::kWatchListTableId));
        if (table != window->second.tables.end()) {
            needs_change_from_close = std::ranges::any_of(
                table->second.columns,
                [](const workstation::ColumnState& column) {
                    return column.visible &&
                           column.id == "change_from_close";
                });
        }
    }
    application::UiWatchListQuery watch_query{
        .document_id = document->id,
        .needs_change_from_close = needs_change_from_close,
    };
    for (const workstation::WatchListRowState& row : document->rows) {
        if (!row.instrument_id.empty() && !row.symbol.empty()) {
            query.market_symbols.push_back(row.symbol);
            watch_query.rows.push_back({
                .row_id = row.id,
                .instrument_id = row.instrument_id,
                .symbol = row.symbol,
            });
        } else if (row.instrument_id.empty() && !row.ticker_input.empty()) {
            query.asset_searches.push_back(row.ticker_input);
        }
    }
    if (!query.asset_searches.empty())
        query.asset_limit = std::max<std::size_t>(query.asset_limit, 8U);
    query.watch_lists.push_back(watch_query);
    queries_.emplace(document->id, std::move(watch_query));
    query.asset_preferred_instrument_ids = state.asset_selection_history;
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
    const application::ApplicationUiSnapshot& snapshot, ImFont* mono,
    ImFont* icons) {
    (void)mono;
#if 0
    // Deliberately empty first pass. The watch-list window should establish
    // its visual shell before any document, table, or market-data behavior is
    // connected to it.
    (void)icons;
    EnsureSession(state);
    workstation::WatchListDocumentState* document = ActiveDocument(state);
    if (document == nullptr) return;
    const auto window_state = state.windows.find(
        std::string(workstation::kWatchListWindowId));
    if (window_state == state.windows.end() || !window_state->second.open)
        return;
    const auto* watch_snapshot = SnapshotFor(snapshot, document->id);
    ui::WorkspaceWindow window{
        .title = "Watch List",
        .id = std::string(workstation::kWatchListWindowId),
        .default_offset = {72.0f, 72.0f},
        .default_size = {720.0f, 480.0f},
        .open = true,
        .flags = ImGuiWindowFlags_NoCollapse,
    };
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (mono != nullptr) ImGui::PushFont(mono);
    if (!workspace.BeginWindow(window)) {
        workspace.EndWindow(window);
        if (mono != nullptr) ImGui::PopFont();
        ImGui::PopStyleVar();
        return;
    }

    constexpr ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_NoSavedSettings;
    std::optional<std::string> pending_delete_row;
    if (ImGui::BeginTable("watch_list_table", 5, table_flags,
                          ImVec2(-FLT_MIN, 0.0f))) {
        constexpr float minimum_column_width = 96.0f;
        ImGui::TableSetupColumn(
            "##watch_list_row_actions",
            ImGuiTableColumnFlags_WidthFixed |
                ImGuiTableColumnFlags_NoHeaderLabel |
                ImGuiTableColumnFlags_NoSort,
            28.0f);
        ImGui::TableSetupColumn(
            "Ticker", ImGuiTableColumnFlags_WidthFixed, minimum_column_width);
        ImGui::TableSetupColumn(
            "Last", ImGuiTableColumnFlags_WidthFixed,
            minimum_column_width);
        ImGui::TableSetupColumn(
            "Close", ImGuiTableColumnFlags_WidthFixed,
            minimum_column_width);
        ImGui::TableSetupColumn(
            "Open", ImGuiTableColumnFlags_WidthFixed,
            minimum_column_width);
        ImGui::TableHeadersRow();

        std::vector<std::size_t> row_indices(document->rows.size());
        std::iota(row_indices.begin(), row_indices.end(), 0U);
        if (const ImGuiTableSortSpecs* sort_specs =
                ImGui::TableGetSortSpecs();
            sort_specs != nullptr && sort_specs->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs& spec = sort_specs->Specs[0];
            const bool descending =
                spec.SortDirection == ImGuiSortDirection_Descending;
            const auto value_for = [&](const workstation::WatchListRowState& row)
                -> std::optional<core::Decimal> {
                const auto* row_snapshot = RowSnapshotFor(watch_snapshot, row.id);
                if (row_snapshot == nullptr) return std::nullopt;
                if (spec.ColumnIndex == 2) return row_snapshot->current_price;
                if (spec.ColumnIndex == 3)
                    return row_snapshot->change_from_previous_close_percent;
                if (spec.ColumnIndex == 4)
                    return row_snapshot->change_from_session_open_percent;
                return std::nullopt;
            };
            const auto empty_row = [](const auto& row) {
                return row.instrument_id.empty() && row.symbol.empty() &&
                       row.ticker_input.empty();
            };
            std::ranges::stable_sort(row_indices, [&](std::size_t left_index,
                                                       std::size_t right_index) {
                const auto& left = document->rows[left_index];
                const auto& right = document->rows[right_index];
                const bool left_empty = empty_row(left);
                const bool right_empty = empty_row(right);
                if (left_empty != right_empty) return !left_empty;
                if (spec.ColumnIndex == 1 && left.symbol != right.symbol)
                    return descending ? left.symbol > right.symbol
                                      : left.symbol < right.symbol;
                const auto left_value = value_for(left);
                const auto right_value = value_for(right);
                if (left_value && right_value && *left_value != *right_value)
                    return descending ? *left_value > *right_value
                                      : *left_value < *right_value;
                if (left_value.has_value() != right_value.has_value())
                    return left_value.has_value();
                return left.id < right.id;
            });
        }

        for (const std::size_t row_index : row_indices) {
            workstation::WatchListRowState& row = document->rows[row_index];
            ImGui::PushID(row.id.c_str());
            ImGui::TableNextRow();
            const bool selected_row =
                !row.symbol.empty() && row.symbol == state.selected_symbol;
            if (selected_row) {
                ImGui::TableSetBgColor(
                    ImGuiTableBgTarget_RowBg0,
                    ImGui::GetColorU32(ImGuiCol_Header));
            }
            ImGui::TableNextColumn();
            const bool empty_row = row.instrument_id.empty() && row.symbol.empty() &&
                                   row.ticker_input.empty();
            if (!empty_row && ImGui::SmallButton("X##delete_row"))
                pending_delete_row = row.id;
            ImGui::TableNextColumn();
            RowInteraction& interaction = interactions_[row.id];
            if (!interaction.initialized) {
                const auto count = std::min(
                    row.ticker_input.size(), interaction.ticker.size() - 1U);
                std::copy_n(row.ticker_input.data(), count,
                            interaction.ticker.data());
                interaction.ticker[count] = '\0';
                interaction.initialized = true;
            }
            interaction.navigation_direction = 0;
            if (!row.symbol.empty()) {
                if (ImGui::Selectable(
                        row.symbol.c_str(), selected_row, 0,
                        {ImGui::GetContentRegionAvail().x, 0.0f})) {
                    state.selected_symbol = row.symbol;
                    persistent_changed_ = true;
                }
            } else {
                if (focus_row_id_ == row.id) {
                    ImGui::SetKeyboardFocusHere();
                    focus_row_id_.clear();
                }
                if (ImGui::InputText(
                        "##ticker", interaction.ticker.data(),
                        interaction.ticker.size(),
                        ImGuiInputTextFlags_CharsUppercase |
                            ImGuiInputTextFlags_CallbackHistory,
                        WatchListTickerInputCallback,
                        &interaction.navigation_direction)) {
                    row.ticker_input = interaction.ticker.data();
                    row.instrument_id.clear();
                    interaction.highlighted_match = -1;
                    persistent_changed_ = true;
                }
            }

            const auto* matches =
                row.instrument_id.empty() && !row.ticker_input.empty()
                    ? MatchesFor(snapshot, row.ticker_input)
                    : nullptr;
            const bool select_with_enter =
                ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter);
            if (matches != nullptr) {
                const std::string popup_id = "##ticker_suggestions_" + row.id;
                const ImVec2 input_min = ImGui::GetItemRectMin();
                ImGui::SetNextWindowPos(
                    ImVec2(input_min.x,
                           input_min.y + ImGui::GetFrameHeight()));
                ImGui::SetNextWindowBgAlpha(0.98f);
                constexpr ImGuiWindowFlags popup_flags =
                    ImGuiWindowFlags_Tooltip |
                    ImGuiWindowFlags_NoDecoration |
                    ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoFocusOnAppearing |
                    ImGuiWindowFlags_NoNav;
                const bool popup_visible =
                    ImGui::Begin(popup_id.c_str(), nullptr, popup_flags);
                if (popup_visible) {
                    if (matches->matches.empty()) {
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.25f, 0.25f, 1.0f),
                            "No ticker found");
                    }
                const std::size_t match_count = matches->matches.size();
                interaction.highlighted_match =
                    SelectedWatchListAutocompleteMatch(
                        interaction.highlighted_match, match_count);
                if (interaction.navigation_direction != 0)
                    interaction.highlighted_match =
                        MoveWatchListAutocompleteMatch(
                            interaction.highlighted_match, match_count,
                            interaction.navigation_direction);
                for (std::size_t match_index = 0;
                     match_index < match_count; ++match_index) {
                    const core::TradableAsset& asset =
                        matches->matches[match_index];
                    const std::string label = asset.symbol + " - " + asset.name;
                    const bool selected = interaction.highlighted_match ==
                                          static_cast<int>(match_index);
                    if (selected) ImGui::SetItemDefaultFocus();
                    if (!ImGui::Selectable(
                            label.c_str(), selected) &&
                        !(select_with_enter && selected))
                        continue;
                    const auto assigned =
                        document->id == workstation::kWatchListDraftId
                            ? workstation::AssignWatchListRowAsset(
                                  *document, row.id, asset.instrument_id,
                                  asset.symbol)
                            : workstation::AssignWatchListRowAsset(
                                  state, document->id, row.id,
                                  asset.instrument_id, asset.symbol);
                    if (!assigned) continue;
                    const auto count = std::min(
                        asset.symbol.size(), interaction.ticker.size() - 1U);
                    std::copy_n(asset.symbol.data(), count,
                                interaction.ticker.data());
                    interaction.ticker[count] = '\0';
                    interaction.highlighted_match = -1;
                    state.selected_symbol = asset.symbol;
                    if (row_index + 1U == document->rows.size()) {
                        if (document->id == workstation::kWatchListDraftId) {
                            document->rows.push_back({
                                .id = workstation::NewStableId("watch-list-row")});
                            focus_row_id_ = document->rows.back().id;
                        } else {
                            const auto added = workstation::AddWatchListRow(
                                state, document->id);
                            if (added) focus_row_id_ = *added;
                        }
                    }
                    persistent_changed_ = true;
                }
                }
                ImGui::End();
            }
            if (select_with_enter &&
                (matches == nullptr || matches->matches.empty())) {
                row.ticker_input.clear();
                row.instrument_id.clear();
                row.symbol.clear();
                interaction.ticker[0] = '\0';
                interaction.highlighted_match = -1;
                focus_row_id_ = row.id;
                persistent_changed_ = true;
            }
            ImGui::TableNextColumn();
            const auto* row_snapshot = RowSnapshotFor(watch_snapshot, row.id);
            ImGui::TextUnformatted(
                row_snapshot != nullptr && row_snapshot->current_price
                    ? row_snapshot->current_price->ToString().c_str()
                    : "--");
            if (!row.symbol.empty() && ImGui::IsItemClicked()) {
                state.selected_symbol = row.symbol;
                persistent_changed_ = true;
            }
            ImGui::TableNextColumn();
            DrawPercentage(
                row_snapshot == nullptr
                    ? std::optional<core::Decimal>{}
                    : row_snapshot->change_from_previous_close_percent);
            if (!row.symbol.empty() && ImGui::IsItemClicked()) {
                state.selected_symbol = row.symbol;
                persistent_changed_ = true;
            }
            ImGui::TableNextColumn();
            DrawPercentage(
                row_snapshot == nullptr
                    ? std::optional<core::Decimal>{}
                    : row_snapshot->change_from_session_open_percent);
            if (!row.symbol.empty() && ImGui::IsItemClicked()) {
                state.selected_symbol = row.symbol;
                persistent_changed_ = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (pending_delete_row) {
        const auto deleted = document->id == workstation::kWatchListDraftId
                                 ? workstation::DeleteWatchListRow(
                                       *document, *pending_delete_row)
                                 : workstation::DeleteWatchListRow(
                                       state, document->id, *pending_delete_row);
        if (deleted) {
            interactions_.erase(*pending_delete_row);
            requested_history_.erase(*pending_delete_row);
            if (document->rows.empty()) {
                const auto added = document->id == workstation::kWatchListDraftId
                                       ? workstation::AddWatchListRow(*document)
                                       : workstation::AddWatchListRow(
                                             state, document->id);
                if (added) focus_row_id_ = *added;
            }
            persistent_changed_ = true;
        }
    }
    workspace.EndWindow(window);
    if (mono != nullptr) ImGui::PopFont();
    ImGui::PopStyleVar();
    return;
#endif
#if 1
    workstation::WatchListDocumentState* active_document =
        ActiveDocument(state);
    if (active_document == nullptr) return;
    workstation::WatchListDocumentState& document = *active_document;
    const auto window_state = state.windows.find(
        std::string(workstation::kWatchListWindowId));
    if (window_state == state.windows.end() || !window_state->second.open)
        return;
        auto table = window_state->second.tables.find(
            std::string(workstation::kWatchListTableId));
        if (table == window_state->second.tables.end()) return;
        const auto columns = OrderedColumns(table->second);
        if (columns.empty()) return;

        ui::WorkspaceWindow window{
            .title = document.name,
            .id = std::string(workstation::kWatchListWindowId),
            .default_offset = {72.0f, 72.0f},
            .default_size = {720.0f, 480.0f},
            .open = true,
        };
        workspace.ConstrainNextWindowSize({520.0f, 220.0f});
        if (!workspace.BeginWindow(window)) {
            workspace.EndWindow(window);
            return;
        }

        const auto* watch_snapshot = SnapshotFor(snapshot, document.id);
        ImGui::PushID(document.id.c_str());
        std::optional<std::string> pending_open;
        std::optional<std::string> pending_delete;
        bool pending_new = false;
        bool pending_save = false;
        ImGui::BeginGroup();
        if (icons != nullptr) {
            if (DrawTitleBarToolButton(
                    "##watch_list_save", 0xe161U, icons,
                    {34.0f, ImGui::GetFrameHeight()},
                    ImGui::GetColorU32(ImGuiCol_Text)))
                pending_save = true;
            ImGui::SetItemTooltip(draft_ ? "Save watch list" : "Saved");
        } else if (ImGui::SmallButton("Save##watch_list_save")) {
            pending_save = true;
        }
        ImGui::SameLine();
        if (!editing_name_) {
            if (ImGui::Button(document.name.c_str())) {
                const auto count = std::min(document.name.size(),
                                            name_input_.size() - 1U);
                std::copy_n(document.name.data(), count, name_input_.data());
                name_input_[count] = '\0';
                editing_name_ = true;
                focus_name_input_ = true;
            }
        } else {
            ImGui::SetNextItemWidth(220.0f);
            if (focus_name_input_) {
                ImGui::SetKeyboardFocusHere();
                focus_name_input_ = false;
            }
            const bool submitted = ImGui::InputText(
                "##watch_list_name", name_input_.data(), name_input_.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (submitted || ImGui::IsItemDeactivatedAfterEdit())
                CommitName(state);
        }
        ImGui::SameLine();
        if (ImGui::Button("Saved lists##watch_list_saved_lists"))
            ImGui::OpenPopup("watch_list_saved_lists");
        if (ImGui::BeginPopup("watch_list_saved_lists")) {
            if (ImGui::MenuItem("+ New watch list")) {
                pending_new = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            for (const workstation::WatchListDocumentState& saved :
                 state.watch_lists) {
                ImGui::PushID(saved.id.c_str());
                if (ImGui::Selectable(
                        saved.name.c_str(), saved.id == document.id))
                    pending_open = saved.id;
                ImGui::SameLine();
                if (ImGui::SmallButton("X##delete_watch_list"))
                    pending_delete = saved.id;
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::EndGroup();
        if (!message_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", message_.c_str());
        }
        ImGui::Separator();
        const std::string table_id = "##watch_list_table";
        const ImGuiTableFlags table_flags =
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_ScrollX | ImGuiTableFlags_NoSavedSettings;
        std::optional<workstation::WatchListColumnKind>
            pending_remove_column;
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
                    if (definition != nullptr &&
                        definition->kind !=
                            workstation::WatchListColumnKind::Symbol &&
                        ImGui::BeginPopupContextItem()) {
                        if (ImGui::MenuItem("Remove column"))
                            pending_remove_column = definition->kind;
                        ImGui::EndPopup();
                    }
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
                    for (workstation::ColumnState& column :
                         table->second.columns) {
                        if (&column != &sorted_column &&
                            !column.sort_direction.empty()) {
                            column.sort_direction.clear();
                            persistent_changed_ = true;
                        }
                    }
                    if (sorted_column.sort_direction != next_direction) {
                        sorted_column.sort_direction = next_direction;
                        persistent_changed_ = true;
                    }
                    std::vector<std::string> prior_row_order;
                    prior_row_order.reserve(document.rows.size());
                    for (const auto& row : document.rows)
                        prior_row_order.push_back(row.id);
                    SortRows(document.rows, sorted_column.id,
                             spec.SortDirection, watch_snapshot);
                    const bool order_changed = !std::ranges::equal(
                        document.rows, prior_row_order, {},
                        &workstation::WatchListRowState::id);
                    if (order_changed) persistent_changed_ = true;
                }
            }

            std::optional<std::pair<std::string, std::size_t>> pending_row_move;
            bool pending_append_row = false;
            for (std::size_t row_index = 0; row_index < document.rows.size();
                 ++row_index) {
                workstation::WatchListRowState& row = document.rows[row_index];
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
                        ImGui::SmallButton("::##row_drag_handle");
                        const ImVec2 drag_handle_min = ImGui::GetItemRectMin();
                        const ImVec2 drag_handle_max = ImGui::GetItemRectMax();
                        if (ImGui::BeginDragDropSource()) {
                            ImGui::SetDragDropPayload(
                                kWatchListRowPayload.data(), row.id.c_str(),
                                row.id.size() + 1U);
                            ImGui::TextUnformatted(
                                row.symbol.empty() ? "Empty watch-list row"
                                                    : row.symbol.c_str());
                            ImGui::EndDragDropSource();
                        }
                        if (ImGui::BeginDragDropTarget()) {
                            const ImGuiPayload* payload =
                                ImGui::AcceptDragDropPayload(
                                    kWatchListRowPayload.data());
                            if (payload != nullptr && payload->DataSize > 1U &&
                                payload->Delivery) {
                                const std::string_view source_row_id(
                                    static_cast<const char*>(payload->Data),
                                    payload->DataSize - 1U);
                                if (source_row_id != row.id) {
                                    const float row_center =
                                        (drag_handle_min.y + drag_handle_max.y) *
                                        0.5f;
                                    pending_row_move = {
                                        std::string(source_row_id),
                                        row_index +
                                            (ImGui::GetMousePos().y > row_center
                                                 ? 1U
                                                 : 0U)};
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                        ImGui::SameLine();
                        const bool populated = !row.instrument_id.empty();
                        const float clear_button_width =
                            ImGui::CalcTextSize("X").x +
                            ImGui::GetStyle().FramePadding.x * 2.0f;
                        ImGui::SetNextItemWidth(std::max(
                            1.0f, ImGui::GetContentRegionAvail().x -
                                       (populated
                                            ? clear_button_width +
                                                  ImGui::GetStyle().ItemSpacing.x
                                            : 0.0f)));
                        interaction.navigation_direction = 0;
                        if (ImGui::InputText(
                                "##ticker", interaction.ticker.data(),
                                interaction.ticker.size(),
                                ImGuiInputTextFlags_CharsUppercase |
                                    ImGuiInputTextFlags_CallbackHistory,
                                WatchListTickerInputCallback,
                                &interaction.navigation_direction)) {
                            row.ticker_input = interaction.ticker.data();
                            row.instrument_id.clear();
                            row.symbol.clear();
                            interaction.highlighted_match = -1;
                            persistent_changed_ = true;
                        }
                        bool clear_requested = false;
                        if (populated) {
                            ImGui::SameLine();
                            clear_requested = ImGui::SmallButton(
                                "X##clear_symbol");
                        }
                        const bool select_with_enter =
                            ImGui::IsItemFocused() &&
                            ImGui::IsKeyPressed(ImGuiKey_Enter);
                        const auto* matches =
                            row.instrument_id.empty() &&
                                    !row.ticker_input.empty()
                                ? MatchesFor(snapshot, row.ticker_input)
                                : nullptr;
                        if (matches != nullptr && !matches->matches.empty()) {
                            const std::size_t match_count =
                                matches->matches.size();
                            interaction.highlighted_match =
                                SelectedWatchListAutocompleteMatch(
                                    interaction.highlighted_match,
                                    match_count);
                            if (interaction.navigation_direction != 0)
                                interaction.highlighted_match =
                                    MoveWatchListAutocompleteMatch(
                                        interaction.highlighted_match,
                                        match_count,
                                        interaction.navigation_direction);
                            for (std::size_t index = 0;
                                 index < match_count; ++index) {
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
                                    document.id == workstation::kWatchListDraftId
                                        ? workstation::AssignWatchListRowAsset(
                                              document, row.id,
                                              asset.instrument_id, asset.symbol)
                                        : workstation::AssignWatchListRowAsset(
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
                                    if (&row == &document.rows.back())
                                        pending_append_row = true;
                                    persistent_changed_ = true;
                                }
                            }
                        }
                        if (clear_requested) {
                            const auto cleared =
                                document.id == workstation::kWatchListDraftId
                                    ? workstation::ClearWatchListRowAsset(
                                          document, row.id)
                                    : workstation::ClearWatchListRowAsset(
                                          state, document.id, row.id);
                            if (cleared) {
                                interaction.ticker[0] = '\0';
                                interaction.highlighted_match = -1;
                                interaction.navigation_direction = 0;
                                persistent_changed_ = true;
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
                    } else if (column_id == "change_from_close") {
                        const auto* row_snapshot =
                            RowSnapshotFor(watch_snapshot, row.id);
                        const std::string value =
                            row_snapshot != nullptr &&
                                    row_snapshot->change_from_close
                                ? SignedDecimal(
                                      *row_snapshot->change_from_close)
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

            if (pending_row_move) {
                const auto moved =
                    document.id == workstation::kWatchListDraftId
                        ? workstation::MoveWatchListRow(
                              document, pending_row_move->first,
                              pending_row_move->second)
                        : workstation::MoveWatchListRow(
                              state, document.id, pending_row_move->first,
                              pending_row_move->second);
                if (moved && *moved) persistent_changed_ = true;
            }
            if (pending_append_row) {
                if (document.id == workstation::kWatchListDraftId) {
                    document.rows.push_back({
                        .id = workstation::NewStableId("watch-list-row")});
                } else if (workstation::AddWatchListRow(
                               state, document.id)) {
                    persistent_changed_ = true;
                }
            }

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

        if (pending_remove_column) {
            const auto removed = workstation::RemoveWatchListColumn(
                state, document.id, *pending_remove_column);
            if (removed) persistent_changed_ = true;
            else message_ = removed.error().message;
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
            const auto added = document.id == workstation::kWatchListDraftId
                                   ? workstation::AddWatchListRow(document)
                                   : workstation::AddWatchListRow(
                                         state, document.id);
            if (added && document.id != workstation::kWatchListDraftId)
                persistent_changed_ = true;
        }
        ImGui::PopID();
        workspace.EndWindow(window);
        if (pending_new) StartNewDraft(state);
        if (pending_save) SaveCurrentDraft(state);
        if (pending_open) OpenSavedDocument(state, *pending_open);
        if (pending_delete) DeleteSavedDocument(state, *pending_delete);
#endif
}

bool WatchListWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

}  // namespace tradebox::gui
