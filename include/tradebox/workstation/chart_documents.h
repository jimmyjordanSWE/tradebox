#pragma once

#include "tradebox/workstation/state.h"

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tradebox::workstation {

struct ChartDocumentError {
    std::string message;
};

struct CreateChartDocumentRequest {
    std::string instrument_id;
    std::string symbol;
};

[[nodiscard]] std::expected<std::string, ChartDocumentError>
CreateChartDocument(WorkspaceState& workspace,
                    CreateChartDocumentRequest request = {});
[[nodiscard]] std::expected<void, ChartDocumentError>
AssignChartInstrument(WorkspaceState& workspace,
                      std::string_view document_id,
                      std::string instrument_id,
                      std::string symbol);
[[nodiscard]] ChartDocumentState* FindChartDocument(
    WorkspaceState& workspace, std::string_view document_id);
[[nodiscard]] const ChartDocumentState* FindChartDocument(
    const WorkspaceState& workspace, std::string_view document_id);
[[nodiscard]] std::expected<void, ChartDocumentError> CloseChartDocument(
    WorkspaceState& workspace, std::string_view document_id);
[[nodiscard]] std::expected<void, ChartDocumentError> ReopenChartDocument(
    WorkspaceState& workspace, std::string_view document_id);
[[nodiscard]] std::expected<void, ChartDocumentError> DeleteChartDocument(
    WorkspaceState& workspace, std::string_view document_id);

[[nodiscard]] std::expected<std::string, ChartDocumentError>
SaveIndicatorSuiteFromChart(WorkspaceState& workspace,
                            std::string_view document_id,
                            std::string name);
[[nodiscard]] std::expected<void, ChartDocumentError> ApplyIndicatorSuite(
    WorkspaceState& workspace, std::string_view document_id,
    std::string_view suite_id);
[[nodiscard]] std::expected<void, ChartDocumentError>
SetChartDefaultsFromDocument(WorkspaceState& workspace,
                             std::string_view document_id);
[[nodiscard]] std::expected<std::string, ChartDocumentError>
AddChartIndicator(WorkspaceState& workspace,
                  std::string_view document_id,
                  ChartIndicatorState indicator);
[[nodiscard]] bool RemoveChartIndicator(
    WorkspaceState& workspace, std::string_view document_id,
    std::string_view indicator_id);

[[nodiscard]] std::expected<std::string, ChartDocumentError>
CreateChartDrawing(WorkspaceState& workspace, ChartDrawingState drawing);
[[nodiscard]] std::expected<void, ChartDocumentError> UpsertChartDrawing(
    WorkspaceState& workspace, ChartDrawingState drawing);
[[nodiscard]] bool DeleteChartDrawing(WorkspaceState& workspace,
                                      std::string_view drawing_id);
[[nodiscard]] std::vector<std::reference_wrapper<const ChartDrawingState>>
DrawingsForInstrument(const WorkspaceState& workspace,
                      std::string_view instrument_id);

}  // namespace tradebox::workstation
