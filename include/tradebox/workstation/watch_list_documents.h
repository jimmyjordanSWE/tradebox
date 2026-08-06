#pragma once

#include "tradebox/workstation/state.h"
#include "tradebox/workstation/watch_list_columns.h"

#include <expected>
#include <string>
#include <string_view>

namespace tradebox::workstation {

struct WatchListDocumentError {
    std::string message;
};

[[nodiscard]] std::expected<std::string, WatchListDocumentError>
CreateWatchListDocument(WorkspaceState& workspace);
[[nodiscard]] WatchListDocumentState* FindWatchListDocument(
    WorkspaceState& workspace, std::string_view document_id);
[[nodiscard]] const WatchListDocumentState* FindWatchListDocument(
    const WorkspaceState& workspace, std::string_view document_id);
[[nodiscard]] std::expected<std::string, WatchListDocumentError>
AddWatchListRow(WorkspaceState& workspace, std::string_view document_id);
[[nodiscard]] std::expected<void, WatchListDocumentError>
AddWatchListColumn(WorkspaceState& workspace, std::string_view document_id,
                   WatchListColumnKind kind);
[[nodiscard]] std::expected<void, WatchListDocumentError>
AssignWatchListRowAsset(WorkspaceState& workspace,
                        std::string_view document_id,
                        std::string_view row_id,
                        std::string instrument_id,
                        std::string symbol);
[[nodiscard]] std::expected<void, WatchListDocumentError>
CloseWatchListDocument(WorkspaceState& workspace,
                       std::string_view document_id);

}  // namespace tradebox::workstation
