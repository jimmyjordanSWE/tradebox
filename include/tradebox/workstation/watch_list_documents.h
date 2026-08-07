#pragma once

#include "tradebox/workstation/state.h"
#include "tradebox/workstation/watch_list_columns.h"

#include <expected>
#include <cstddef>
#include <string>
#include <string_view>

namespace tradebox::workstation {

inline constexpr std::string_view kWatchListWindowId = "watch-list.window";
inline constexpr std::string_view kWatchListDraftId = "watch-list.draft";
inline constexpr std::string_view kWatchListDefaultId = "watch-list.default";

struct WatchListDocumentError {
    std::string message;
};

[[nodiscard]] std::expected<std::string, WatchListDocumentError>
CreateWatchListDocument(WorkspaceState& workspace);
[[nodiscard]] std::expected<void, WatchListDocumentError>
EnsureWatchListWindow(WorkspaceState& workspace);
[[nodiscard]] std::expected<std::string, WatchListDocumentError>
EnsureDefaultWatchList(WorkspaceState& workspace);
[[nodiscard]] std::expected<std::string, WatchListDocumentError>
SaveWatchListDocument(WorkspaceState& workspace,
                      WatchListDocumentState document);
[[nodiscard]] std::expected<void, WatchListDocumentError>
OpenWatchListDocument(WorkspaceState& workspace, std::string_view document_id);
[[nodiscard]] std::expected<void, WatchListDocumentError>
RenameWatchListDocument(WorkspaceState& workspace,
                        std::string_view document_id, std::string name);
[[nodiscard]] std::expected<void, WatchListDocumentError>
DeleteWatchListDocument(WorkspaceState& workspace,
                        std::string_view document_id);
[[nodiscard]] WatchListDocumentState* FindWatchListDocument(
    WorkspaceState& workspace, std::string_view document_id);
[[nodiscard]] const WatchListDocumentState* FindWatchListDocument(
    const WorkspaceState& workspace, std::string_view document_id);
[[nodiscard]] std::expected<std::string, WatchListDocumentError>
AddWatchListRow(WorkspaceState& workspace, std::string_view document_id);
[[nodiscard]] std::expected<std::string, WatchListDocumentError>
AddWatchListRow(WatchListDocumentState& document);
// Maintains exactly one fully empty row at the end of a document. This is the
// durable draft row used to add the next symbol.
[[nodiscard]] bool EnsureWatchListTrailingEmptyRow(
    WatchListDocumentState& document);
[[nodiscard]] std::expected<void, WatchListDocumentError>
DeleteWatchListRow(WorkspaceState& workspace, std::string_view document_id,
                   std::string_view row_id);
[[nodiscard]] std::expected<void, WatchListDocumentError>
DeleteWatchListRow(WatchListDocumentState& document,
                   std::string_view row_id);
[[nodiscard]] std::expected<void, WatchListDocumentError>
AddWatchListColumn(WorkspaceState& workspace, std::string_view document_id,
                   WatchListColumnKind kind);
[[nodiscard]] std::expected<void, WatchListDocumentError>
RemoveWatchListColumn(WorkspaceState& workspace, std::string_view document_id,
                      WatchListColumnKind kind);
[[nodiscard]] std::expected<void, WatchListDocumentError>
AssignWatchListRowAsset(WorkspaceState& workspace,
                        std::string_view document_id,
                        std::string_view row_id,
                        std::string instrument_id,
                        std::string symbol);
[[nodiscard]] std::expected<void, WatchListDocumentError>
AssignWatchListRowAsset(WatchListDocumentState& document,
                        std::string_view row_id, std::string instrument_id,
                        std::string symbol);
[[nodiscard]] std::expected<void, WatchListDocumentError>
ClearWatchListRowAsset(WorkspaceState& workspace,
                       std::string_view document_id,
                       std::string_view row_id);
[[nodiscard]] std::expected<void, WatchListDocumentError>
ClearWatchListRowAsset(WatchListDocumentState& document,
                       std::string_view row_id);
// The insertion index is measured in the original row vector. Moving a row
// to an index after its current position accounts for the removed row.
[[nodiscard]] std::expected<bool, WatchListDocumentError>
MoveWatchListRow(WorkspaceState& workspace, std::string_view document_id,
                 std::string_view row_id, std::size_t insertion_index);
[[nodiscard]] std::expected<bool, WatchListDocumentError>
MoveWatchListRow(WatchListDocumentState& document, std::string_view row_id,
                 std::size_t insertion_index);
[[nodiscard]] std::expected<void, WatchListDocumentError>
CloseWatchListDocument(WorkspaceState& workspace,
                       std::string_view document_id);

}  // namespace tradebox::workstation
