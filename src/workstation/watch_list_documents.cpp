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
    const std::size_t number = workspace.watch_lists.size() + 1U;
    WatchListDocumentState document{
        .id = id,
        .name = "Watch List " + std::to_string(number),
        .rows = {{.id = NewStableId("watch-list-row")}},
    };
    const std::string document_id = document.id;
    workspace.watch_lists.push_back(std::move(document));
    workspace.windows.emplace(
        document_id,
        WindowInstanceState{
            .id = document_id,
            .kind = "watch-list",
            .title = workspace.watch_lists.back().name,
            .open = true,
            .bounds = {72.0f, 72.0f, 720.0f, 480.0f},
        });
    return document_id;
}

std::expected<std::string, WatchListDocumentError>
AddWatchListRow(WorkspaceState& workspace, std::string_view document_id) {
    WatchListDocumentState* document = FindDocument(workspace, document_id);
    if (document == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    const std::string row_id = NewStableId("watch-list-row");
    document->rows.push_back({.id = row_id});
    return row_id;
}

std::expected<void, WatchListDocumentError> AssignWatchListRowAsset(
    WorkspaceState& workspace, std::string_view document_id,
    std::string_view row_id, std::string instrument_id, std::string symbol) {
    if (instrument_id.empty() || symbol.empty())
        return std::unexpected(WatchListDocumentError{
            "watch list row assignment requires stable identity and symbol"});
    WatchListDocumentState* document = FindDocument(workspace, document_id);
    if (document == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list document does not exist"});
    WatchListRowState* row = FindRow(*document, row_id);
    if (row == nullptr)
        return std::unexpected(
            WatchListDocumentError{"watch list row does not exist"});
    row->instrument_id = std::move(instrument_id);
    row->symbol = std::move(symbol);
    row->ticker_input = row->symbol;
    return {};
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
