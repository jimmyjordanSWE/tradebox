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
#include <ctime>
#include <format>
#include <iomanip>
#include <optional>
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

constexpr auto kWatchListTableChoices = [] {
    std::array<TableColumnChoice,
               workstation::kWatchListColumnDefinitions.size()> choices{};
    for (std::size_t index = 0; index < choices.size(); ++index) {
        const auto& definition =
            workstation::kWatchListColumnDefinitions[index];
        choices[index] = {
            definition.id, definition.label,
            definition.kind == workstation::WatchListColumnKind::Symbol};
    }
    return choices;
}();

std::string Money(const core::Decimal& value) {
    return std::format("${:.2f}", value.ToDisplayDouble());
}

std::string TradeTime(std::int64_t timestamp_ns) {
    if (timestamp_ns <= 0) return "--";
    const std::time_t seconds = static_cast<std::time_t>(
        timestamp_ns / 1'000'000'000);
    std::tm local{};
    localtime_s(&local, &seconds);
    std::ostringstream value;
    value << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return value.str();
}

std::string SignedMoney(const core::Decimal& value) {
    return std::format("{}${:.2f}", value > core::Decimal::Zero() ? "+" : "",
                       value.ToDisplayDouble());
}

std::string Percent(const core::Decimal& value) {
    return std::format("{:.2f}%", value.ToDisplayDouble());
}

ImVec4 UnpackColor(std::uint32_t packed) {
    return {
        static_cast<float>((packed >> 24U) & 0xffU) / 255.0f,
        static_cast<float>((packed >> 16U) & 0xffU) / 255.0f,
        static_cast<float>((packed >> 8U) & 0xffU) / 255.0f,
        static_cast<float>(packed & 0xffU) / 255.0f};
}

ImVec4 WatchListPercentColor(const core::Decimal& value,
                             const workstation::ApplicationSettings& settings) {
    const double percent = value.ToDisplayDouble();
    if (percent > 3.0) return UnpackColor(settings.watch_list_strong_green_rgba);
    if (percent > 0.0) return UnpackColor(settings.watch_list_light_green_rgba);
    if (percent < -3.0) return UnpackColor(settings.watch_list_strong_red_rgba);
    if (percent < 0.0) return UnpackColor(settings.watch_list_light_red_rgba);
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
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
    if (column_id == "trade_time" && row_snapshot->current_price_time_ns > 0) {
        const auto parsed = core::Decimal::Parse(
            std::to_string(row_snapshot->current_price_time_ns));
        if (parsed) return *parsed;
    }
    if (column_id == "session_open") return row_snapshot->session_open;
    if (column_id == "previous_close") return row_snapshot->previous_close;
    if (column_id == "change_from_open")
        return row_snapshot->change_from_open;
    if (column_id == "change_from_open_percent")
        return row_snapshot->change_from_session_open_percent;
    return std::nullopt;
}

std::vector<workstation::WatchListRowState*> SortedRows(
    std::vector<workstation::WatchListRowState>& rows,
    std::string_view column_id, ImGuiSortDirection direction,
    const application::UiWatchListSnapshot* snapshot) {
    std::vector<workstation::WatchListRowState*> result;
    result.reserve(rows.size());
    for (workstation::WatchListRowState& row : rows)
        result.push_back(&row);
    if (direction == ImGuiSortDirection_None) return result;
    const bool descending = direction == ImGuiSortDirection_Descending;
    std::ranges::stable_sort(result, [&](const auto* left, const auto* right) {
        if (column_id == "symbol") {
            if (left->symbol != right->symbol)
                return descending ? left->symbol > right->symbol
                                   : left->symbol < right->symbol;
            return left->id < right->id;
        }
        const auto left_value = NumericValueFor(*left, column_id, snapshot);
        const auto right_value = NumericValueFor(*right, column_id, snapshot);
        if (left_value && right_value && *left_value != *right_value) {
            return descending ? *left_value > *right_value
                              : *left_value < *right_value;
        }
        if (left_value.has_value() != right_value.has_value())
            return left_value.has_value();
        return left->id < right->id;
    });
    return result;
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
    const auto* default_document = workstation::FindWatchListDocument(
        state, workstation::kWatchListDefaultId);
    const bool requires_legacy_migration =
        !state.watchlist.empty() &&
        (default_document == nullptr || default_document->rows.empty());
    const auto ensured = workstation::EnsureDefaultWatchList(state);
    if (!ensured) {
        message_ = ensured.error().message;
        return;
    }
    if (requires_legacy_migration) persistent_changed_ = true;
    if (!session_normalized_) {
        const auto removed = workstation::RemoveEmptyWatchListRows(
            state, workstation::kWatchListDefaultId);
        if (!removed) {
            message_ = removed.error().message;
            return;
        }
        if (*removed) persistent_changed_ = true;
        session_normalized_ = true;
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
    workstation::WatchListDocumentState* document = ActiveDocument(state);
    if (document == nullptr) return;
    const auto window = state.windows.find(
        std::string(workstation::kWatchListWindowId));
    bool needs_change_from_open = false;
    if (window != state.windows.end()) {
        const auto table = window->second.tables.find(
            std::string(workstation::kWatchListTableId));
        if (table != window->second.tables.end()) {
            needs_change_from_open = std::ranges::any_of(
                table->second.columns,
                [](const workstation::ColumnState& column) {
                    return column.visible &&
                           (column.id == "session_open" ||
                            column.id == "previous_close" ||
                            column.id == "change_from_open" ||
                            column.id == "change_from_open_percent");
                });
        }
    }
    application::UiWatchListQuery watch_query{
        .document_id = document->id,
        .needs_change_from_open = needs_change_from_open,
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
    query.asset_preferred_instrument_ids = state.asset_selection_history;
}

void WatchListWindowRenderer::Draw(
    ui::Workspace& workspace, workstation::WorkspaceState& state,
    const application::ApplicationUiSnapshot& snapshot,
    const workstation::ApplicationSettings& settings, ImFont* mono,
    ImFont* icons) {
    (void)mono;
    (void)icons;
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
        const auto columns = OrderedVisibleTableColumns(table->second);
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
        ImGui::TextUnformatted("Watch List");
        ImGui::SameLine();
        ImGui::TextDisabled("Double-click a ticker to open Order Ticket");
        ImGui::SameLine();
        if (ImGui::Button("Add Ticker##watch_list_add_ticker")) {
            const auto added = workstation::AddWatchListRow(state, document.id);
            if (added) {
                focus_row_id_ = *added;
                persistent_changed_ = true;
            }
        }
        ImGui::SameLine();
        const TableColumnActions column_actions = DrawTableColumnControls(
            table->second, kWatchListTableChoices, "watch_list_columns");
        if (!message_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", message_.c_str());
        }
        ImGui::Separator();
        const float table_height = std::max(
            60.0f, ImGui::GetContentRegionAvail().y -
                       ImGui::GetFrameHeightWithSpacing());
        const std::string table_id = "##watch_list_table";
        const ImGuiTableFlags table_flags =
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
            ImGuiTableFlags_Sortable |
            ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_NoSavedSettings;
        std::optional<std::string> pending_delete_row;
        if (ImGui::BeginTable(table_id.c_str(),
                              static_cast<int>(columns.size()), table_flags,
                              ImVec2(0.0f, table_height))) {
            SetupPersistentTableColumns(
                columns, kWatchListTableChoices, 140.0f);
            std::vector<std::string> display_column_ids;
            display_column_ids.reserve(columns.size());
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (std::size_t index = 0; index < columns.size(); ++index) {
                ImGui::TableNextColumn();
                const std::string id =
                    TableColumnIdFromLabel(ImGui::TableGetColumnName());
                display_column_ids.push_back(id);
                ImGui::TableHeader(ImGui::TableGetColumnName());
            }

            persistent_changed_ =
                PersistTableSortSpecs(
                    table->second, ImGui::TableGetSortSpecs()) ||
                persistent_changed_;

            const auto sorted_column = std::ranges::find_if(
                columns, [](const workstation::ColumnState* column) {
                    return !column->sort_direction.empty();
                });
            const std::vector<workstation::WatchListRowState*> display_rows =
                sorted_column == columns.end()
                    ? SortedRows(document.rows, "symbol",
                                 ImGuiSortDirection_None, watch_snapshot)
                    : SortedRows(
                          document.rows, (*sorted_column)->id,
                          (*sorted_column)->sort_direction == "descending"
                              ? ImGuiSortDirection_Descending
                              : ImGuiSortDirection_Ascending,
                          watch_snapshot);
            for (workstation::WatchListRowState* row_pointer : display_rows) {
                workstation::WatchListRowState& row = *row_pointer;
                ImGui::PushID(row.id.c_str());
                ImGui::TableNextRow();
                for (const std::string& column_id : display_column_ids) {
                    ImGui::TableNextColumn();
                    if (column_id == "symbol") {
                        RowInteraction& interaction = interactions_[row.id];
                        const bool populated = !row.instrument_id.empty();
                        if (populated) {
                            const bool selected = ImGui::Selectable(
                                row.symbol.c_str(),
                                row.symbol == state.selected_symbol);
                            if (selected) {
                                state.selected_symbol = row.symbol;
                                persistent_changed_ = true;
                            }
                            ImGui::SameLine();
                            const bool clear_requested = ImGui::SmallButton(
                                "X##clear_symbol");
                            if (clear_requested) {
                                pending_delete_row = row.id;
                                // Remove after EndTable so the loop never
                                // invalidates its current row reference.
                            }
                        } else {
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
                        if (focus_row_id_ == row.id) {
                            ImGui::SetKeyboardFocusHere();
                            focus_row_id_.clear();
                        }
                        interaction.navigation_direction = 0;
                        if (ImGui::InputText(
                                "##add_ticker", interaction.ticker.data(),
                                interaction.ticker.size(),
                                ImGuiInputTextFlags_CharsUppercase |
                                    ImGuiInputTextFlags_CallbackHistory,
                                WatchListTickerInputCallback,
                                &interaction.navigation_direction)) {
                            row.ticker_input = interaction.ticker.data();
                            interaction.highlighted_match = -1;
                            persistent_changed_ = true;
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
                                    persistent_changed_ = true;
                                }
                            }
                        }
                        }
                    } else if (column_id == "current_price") {
                        const auto* row_snapshot =
                            RowSnapshotFor(watch_snapshot, row.id);
                        ImGui::TextUnformatted(
                            row_snapshot != nullptr &&
                                row_snapshot->current_price
                                ? Money(*row_snapshot->current_price).c_str()
                                : "--");
                    } else if (column_id == "trade_time") {
                        const auto* row_snapshot =
                            RowSnapshotFor(watch_snapshot, row.id);
                        const std::string value =
                            row_snapshot == nullptr
                                ? "--"
                                : TradeTime(row_snapshot->current_price_time_ns);
                        ImGui::TextUnformatted(value.c_str());
                    } else if (column_id == "session_open") {
                        const auto* row_snapshot =
                            RowSnapshotFor(watch_snapshot, row.id);
                        ImGui::TextUnformatted(
                            row_snapshot != nullptr && row_snapshot->session_open
                                ? Money(*row_snapshot->session_open).c_str()
                                : "--");
                    } else if (column_id == "previous_close") {
                        const auto* row_snapshot =
                            RowSnapshotFor(watch_snapshot, row.id);
                        ImGui::TextUnformatted(
                            row_snapshot != nullptr && row_snapshot->previous_close
                                ? Money(*row_snapshot->previous_close).c_str()
                                : "--");
                    } else if (column_id == "change_from_open") {
                        const auto* row_snapshot =
                            RowSnapshotFor(watch_snapshot, row.id);
                        const std::string value =
                            row_snapshot != nullptr &&
                                    row_snapshot->change_from_open
                                ? SignedMoney(*row_snapshot->change_from_open)
                                : "--";
                        ImGui::TextUnformatted(value.c_str());
                    } else if (column_id == "change_from_open_percent") {
                        const auto* row_snapshot =
                            RowSnapshotFor(watch_snapshot, row.id);
                        const std::string value =
                            row_snapshot != nullptr &&
                                    row_snapshot->change_from_session_open_percent
                                ? Percent(*row_snapshot->change_from_session_open_percent)
                                : "--";
                        if (row_snapshot != nullptr &&
                            row_snapshot->change_from_session_open_percent) {
                            ImGui::TextColored(
                                WatchListPercentColor(
                                    *row_snapshot->change_from_session_open_percent,
                                    settings),
                                "%s", value.c_str());
                        } else {
                            ImGui::TextUnformatted(value.c_str());
                        }
                    } else {
                        ImGui::TextDisabled("--");
                    }
                }
                ImGui::PopID();
            }

            ImGui::EndTable();

            if (pending_delete_row) {
                const auto deleted = workstation::DeleteWatchListRow(
                    state, document.id, *pending_delete_row);
                if (deleted) {
                    interactions_.erase(*pending_delete_row);
                    persistent_changed_ = true;
                } else {
                    message_ = deleted.error().message;
                }
            }

            persistent_changed_ =
                PersistTableColumnOrder(
                    table->second, display_column_ids) ||
                persistent_changed_;
        }

        if (column_actions.remove) {
            const auto kind = workstation::WatchListColumnKindFromId(
                *column_actions.remove);
            if (!kind) {
                message_ = "Unknown watch list column";
            } else {
                const auto removed = workstation::RemoveWatchListColumn(
                    state, document.id, *kind);
                if (removed)
                    persistent_changed_ = true;
                else
                    message_ = removed.error().message;
            }
        }
        if (column_actions.add) {
            const auto kind = workstation::WatchListColumnKindFromId(
                *column_actions.add);
            if (!kind) {
                message_ = "Unknown watch list column";
            } else {
                const auto added = workstation::AddWatchListColumn(
                    state, document.id, *kind);
                if (added)
                    persistent_changed_ = true;
                else
                    message_ = added.error().message;
            }
        }
        ImGui::PopID();
        workspace.EndWindow(window);
}

bool WatchListWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

}  // namespace tradebox::gui
