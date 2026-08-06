#include "tradebox/workstation/watch_list_documents.h"

#include "tradebox/workstation/stable_id.h"

#include <algorithm>
#include <ranges>

namespace tradebox::workstation {
namespace {

WatchListDocumentState* FindDocument(WorkspaceState& workspace,
                                     std::string_view document_id) {
    const auto found = std::ranges::find(
        workspace.watch_lists, document_id, &WatchListDocumentState::id);
    return found == workspace.watch_lists.end() ? nullptr : &*found;
}

const WatchListDocumentState* FindDocument(
    const WorkspaceState& workspace, std::string_view document_id) {
    const auto found = std::ranges::find(
        workspace.watch_lists, document_id, &WatchListDocumentState::id);
    return found == workspace.watch_lists.end() ? nullptr : &*found;
}

WatchListRowState* FindRow(WatchListDocumentState& document,
                           std::string_view row_id) {
    const auto found = std::ranges::find(
        document.rows, row_id, &WatchListRowState::id);
    return found == document.rows.end() ? nullptr : &*found;
}

std::expected<void, WatchListDocumentError> EnsureWindow(
    WorkspaceState& workspace) {
    for (auto iterator = workspace.windows.begin();
         iterator != workspace.windows.end();) {
        if (iterator->second.kind == "watch-list" &&
            iterator->first != kWatchListWindowId)
            iterator = workspace.windows.erase(iterator);
        else
            ++iterator;
    }
    const auto existing = workspace.windows.find(
        std::string(kWatchListWindowId));
    if (existing != workspace.windows.end() &&
        existing->second.kind != "watch-list")
        return std::unexpected(WatchListDocumentError{
            "watch list window ID is owned by another window"});
    if (existing == workspace.windows.end()) {
        workspace.windows.emplace(
            std::string(kWatchListWindowId),
            WindowInstanceState{
                .id = std::string(kWatchListWindowId),
                .kind = "watch-list",
                .title = "Watch List",
                .open = true,
                .bounds = {72.0f, 72.0f, 720.0f, 480.0f},
            });
    }
    PersistentTableState& table = workspace.windows.at(
        std::string(kWatchListWindowId))
                                      .tables[std::string(kWatchListTableId)];
    if (!std::ranges::any_of(table.columns, [](const ColumnState& column) {
            return column.id == "symbol";
        })) {
        table.columns.insert(table.columns.begin(),
                             {.id = "symbol", .order = 0,
                              .width = 150.0f, .visible = true});
    }
    if (existing == workspace.windows.end()) {
        table.columns.push_back({.id = "current_price", .order = 1,
                                 .width = 140.0f, .visible = true});
        table.columns.push_back({.id = "change_from_close", .order = 2,
                                 .width = 160.0f, .visible = true});
    } else if (workspace.watch_lists.empty() &&
               workspace.active_watch_list_id.empty() &&
               table.columns.size() == 1U) {
        // Upgrade the pre-table empty session once. After the user removes
        // optional columns, the resulting choice is retained.
        table.columns.push_back({.id = "current_price", .order = 1,
                                 .width = 140.0f, .visible = true});
        table.columns.push_back({.id = "change_from_close", .order = 2,
                                 .width = 160.0f, .visible = true});
    }
    return {};
}

bool HasWatchListName(const WorkspaceState& workspace, std::string_view name,
                      std::string_view ignored_id = {}) {
    return std::ranges::any_of(
        workspace.watch_lists, [&](const WatchListDocumentState& document) {
            return document.id != ignored_id && document.name == name;
        });
}

}  // namespace

WatchListDocumentState* FindWatchListDocument(
    WorkspaceState& workspace, std::string_view document_id) {
    return FindDocument(workspace, document_id);
}

const WatchListDocumentState* FindWatchListDocument(
    const WorkspaceState& workspace, std::string_view document_id) {
    return FindDocument(workspace, document_id);
}

std::expected<std::string, WatchListDocumentError>
CreateWatchListDocument(WorkspaceState& workspace) {
    std::string id;
    do {
        id = NewStableId("watch-list");
    } while (FindDocument(workspace, id) != nullptr ||
             workspace.windows.contains(id));
    WatchListDocumentState document{
        .id = id,
        .name = "Untitled Watch List",
        .rows = {{.id = NewStableId("watch-list-row")}},
    };
    const auto saved = SaveWatchListDocument(workspace, std::move(document));
    if (!saved) return std::unexpected(saved.error());
    return *saved;
}

std::expected<void, WatchListDocumentError> EnsureWatchListWindow(
    WorkspaceState& workspace) {
    return EnsureWindow(workspace);
}

std::expected<std::string, WatchListDocumentError> EnsureDefaultWatchList(
    WorkspaceState& workspace) {
    WatchListDocumentState* default_document =
        FindDocument(workspace, kWatchListDefaultId);
    if (default_document == nullptr) {
        workspace.watch_lists.push_back({
            .id = std::string(kWatchListDefaultId),
            .name = "Default",
            .rows = {{.id = NewStableId("watch-list-row")}},
        });
    }
    workspace.active_watch_list_id = std::string(kWatchListDefaultId);
    return std::string(kWatchListDefaultId);
}

std::expected<std::string, WatchListDocumentError> SaveWatchListDocument(
    WorkspaceState& workspace, WatchListDocumentState document) {
    if (document.name.empty())
        return std::unexpected(
            WatchListDocumentError{"watch list name cannot be empty"});
    if (HasWatchListName(workspace, document.name))
        return std::unexpected(
            WatchListDocumentError{"watch list name is already in use"});
    document.id = NewStableId("watch-list");
    for (WatchListRowState& row : document.rows)
        if (row.id.empty()) row.id = NewStableId("watch-list-row");
    workspace.watch_lists.push_back(std::move(document));
    workspace.active_watch_list_id = workspace.watch_lists.back().id;
    const auto ensured = EnsureWindow(workspace);
    if (!ensured) return std::unexpected(ensured.error());
    workspace.windows.at(std::string(kWatchListWindowId)).open = true;
    return workspace.active_watch_list_id;
}

std::expected<void, WatchListDocumentError> OpenWatchListDocument(
    WorkspaceState& workspace, std::string_view document_id) {
    if (FindDocument(workspace, document_id) == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    workspace.active_watch_list_id = std::string(document_id);
    const auto ensured = EnsureWindow(workspace);
    if (!ensured) return std::unexpected(ensured.error());
    workspace.windows.at(std::string(kWatchListWindowId)).open = true;
    return {};
}

std::expected<void, WatchListDocumentError> RenameWatchListDocument(
    WorkspaceState& workspace, std::string_view document_id, std::string name) {
    if (name.empty())
        return std::unexpected(
            WatchListDocumentError{"watch list name cannot be empty"});
    WatchListDocumentState* document = FindDocument(workspace, document_id);
    if (document == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    if (document_id == kWatchListDefaultId)
        return std::unexpected(WatchListDocumentError{
            "the default watch list cannot be renamed"});
    if (HasWatchListName(workspace, name, document_id))
        return std::unexpected(
            WatchListDocumentError{"watch list name is already in use"});
    document->name = std::move(name);
    return {};
}

std::expected<void, WatchListDocumentError> DeleteWatchListDocument(
    WorkspaceState& workspace, std::string_view document_id) {
    const auto found = std::ranges::find(
        workspace.watch_lists, document_id, &WatchListDocumentState::id);
    if (found == workspace.watch_lists.end())
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    if (document_id == kWatchListDefaultId)
        return std::unexpected(WatchListDocumentError{
            "the default watch list cannot be deleted"});
    workspace.watch_lists.erase(found);
    if (workspace.active_watch_list_id == document_id)
        workspace.active_watch_list_id.clear();
    return {};
}

std::expected<std::string, WatchListDocumentError>
AddWatchListRow(WorkspaceState& workspace, std::string_view document_id) {
    WatchListDocumentState* document = FindDocument(workspace, document_id);
    if (document == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    return AddWatchListRow(*document);
}

std::expected<std::string, WatchListDocumentError> AddWatchListRow(
    WatchListDocumentState& document) {
    const std::string row_id = NewStableId("watch-list-row");
    document.rows.push_back({.id = row_id});
    return row_id;
}

std::expected<void, WatchListDocumentError> DeleteWatchListRow(
    WatchListDocumentState& document, std::string_view row_id) {
    const auto found = std::ranges::find(
        document.rows, row_id, &WatchListRowState::id);
    if (found == document.rows.end())
        return std::unexpected(
            WatchListDocumentError{"watch list row does not exist"});
    document.rows.erase(found);
    return {};
}

std::expected<void, WatchListDocumentError> DeleteWatchListRow(
    WorkspaceState& workspace, std::string_view document_id,
    std::string_view row_id) {
    WatchListDocumentState* document = FindDocument(workspace, document_id);
    if (document == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    return DeleteWatchListRow(*document, row_id);
}

std::expected<void, WatchListDocumentError> AddWatchListColumn(
    WorkspaceState& workspace, std::string_view document_id,
    WatchListColumnKind kind) {
    const auto* definition = FindWatchListColumn(kind);
    if (definition == nullptr || kind == WatchListColumnKind::Symbol)
        return std::unexpected(
            WatchListDocumentError{"watch list column is not addable"});
    WatchListDocumentState* document = FindDocument(workspace, document_id);
    if (document == nullptr && document_id != kWatchListDraftId)
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    const auto window = workspace.windows.find(
        std::string(kWatchListWindowId));
    if (window == workspace.windows.end() ||
        window->second.kind != "watch-list")
        return std::unexpected(WatchListDocumentError{
            "watch list document has no matching window"});
    PersistentTableState& table =
        window->second.tables[std::string(kWatchListTableId)];
    if (std::ranges::any_of(table.columns, [&](const ColumnState& column) {
            return column.id == definition->id;
        }))
        return std::unexpected(
            WatchListDocumentError{"watch list column is already present"});
    int next_order = 0;
    for (const ColumnState& column : table.columns)
        next_order = std::max(next_order, column.order + 1);
    table.columns.push_back({
        .id = std::string(definition->id),
        .order = next_order,
        .width = 140.0f,
        .visible = true,
    });
    return {};
}

std::expected<void, WatchListDocumentError> RemoveWatchListColumn(
    WorkspaceState& workspace, std::string_view document_id,
    WatchListColumnKind kind) {
    const auto* definition = FindWatchListColumn(kind);
    if (definition == nullptr || kind == WatchListColumnKind::Symbol)
        return std::unexpected(
            WatchListDocumentError{"watch list column cannot be removed"});
    if (FindDocument(workspace, document_id) == nullptr &&
        document_id != kWatchListDraftId)
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    const auto window = workspace.windows.find(
        std::string(kWatchListWindowId));
    if (window == workspace.windows.end() ||
        window->second.kind != "watch-list")
        return std::unexpected(WatchListDocumentError{
            "watch list document has no matching window"});
    auto& columns = window->second.tables[std::string(kWatchListTableId)].columns;
    const auto found = std::ranges::find(
        columns, definition->id, &ColumnState::id);
    if (found == columns.end())
        return std::unexpected(
            WatchListDocumentError{"watch list column is not present"});
    columns.erase(found);
    for (std::size_t index = 0; index < columns.size(); ++index)
        columns[index].order = static_cast<int>(index);
    return {};
}

std::expected<void, WatchListDocumentError> AssignWatchListRowAsset(
    WatchListDocumentState& document, std::string_view row_id,
    std::string instrument_id, std::string symbol) {
    if (instrument_id.empty() || symbol.empty())
        return std::unexpected(WatchListDocumentError{
            "watch list row assignment requires stable identity and symbol"});
    WatchListRowState* row = FindRow(document, row_id);
    if (row == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list row does not exist"});
    row->instrument_id = std::move(instrument_id);
    row->symbol = std::move(symbol);
    row->ticker_input = row->symbol;
    return {};
}

std::expected<void, WatchListDocumentError> AssignWatchListRowAsset(
    WorkspaceState& workspace, std::string_view document_id,
    std::string_view row_id, std::string instrument_id, std::string symbol) {
    WatchListDocumentState* document = FindDocument(workspace, document_id);
    if (document == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    return AssignWatchListRowAsset(*document, row_id, std::move(instrument_id),
                                   std::move(symbol));
}

std::expected<void, WatchListDocumentError> ClearWatchListRowAsset(
    WatchListDocumentState& document, std::string_view row_id) {
    WatchListRowState* row = FindRow(document, row_id);
    if (row == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list row does not exist"});
    row->instrument_id.clear();
    row->symbol.clear();
    row->ticker_input.clear();
    return {};
}

std::expected<void, WatchListDocumentError> ClearWatchListRowAsset(
    WorkspaceState& workspace, std::string_view document_id,
    std::string_view row_id) {
    WatchListDocumentState* document = FindDocument(workspace, document_id);
    if (document == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    return ClearWatchListRowAsset(*document, row_id);
}

std::expected<bool, WatchListDocumentError> MoveWatchListRow(
    WatchListDocumentState& document, std::string_view row_id,
    std::size_t insertion_index) {
    const auto found = std::ranges::find(
        document.rows, row_id, &WatchListRowState::id);
    if (found == document.rows.end())
        return std::unexpected(
            WatchListDocumentError{"watch list row does not exist"});
    if (insertion_index > document.rows.size())
        return std::unexpected(WatchListDocumentError{
            "watch list row insertion index is out of range"});

    const std::size_t source_index = static_cast<std::size_t>(
        std::distance(document.rows.begin(), found));
    if (insertion_index == source_index ||
        insertion_index == source_index + 1U)
        return false;

    WatchListRowState moved = std::move(*found);
    document.rows.erase(found);
    if (source_index < insertion_index) --insertion_index;
    document.rows.insert(document.rows.begin() +
                             static_cast<std::ptrdiff_t>(insertion_index),
                         std::move(moved));
    return true;
}

std::expected<bool, WatchListDocumentError> MoveWatchListRow(
    WorkspaceState& workspace, std::string_view document_id,
    std::string_view row_id, std::size_t insertion_index) {
    WatchListDocumentState* document = FindDocument(workspace, document_id);
    if (document == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    return MoveWatchListRow(*document, row_id, insertion_index);
}

std::expected<void, WatchListDocumentError> CloseWatchListDocument(
    WorkspaceState& workspace, std::string_view document_id) {
    if (FindDocument(workspace, document_id) == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    const auto window = workspace.windows.find(std::string(document_id));
    if (window == workspace.windows.end() || window->second.kind != "watch-list")
        return std::unexpected(WatchListDocumentError{
            "watch list document has no matching window"});
    window->second.open = false;
    return {};
}

}  // namespace tradebox::workstation
