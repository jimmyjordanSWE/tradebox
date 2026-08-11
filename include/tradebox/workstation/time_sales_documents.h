#pragma once

#include "tradebox/workstation/state.h"

#include <expected>
#include <string>
#include <string_view>

namespace tradebox::workstation {

inline constexpr std::string_view kTimeSalesTableId = "time-sales";

struct TimeSalesDocumentError { std::string message; };

[[nodiscard]] std::expected<std::string, TimeSalesDocumentError>
CreateTimeSalesDocument(WorkspaceState& state);
[[nodiscard]] TimeSalesDocumentState* FindTimeSalesDocument(
    WorkspaceState& state, std::string_view id);
[[nodiscard]] std::expected<void, TimeSalesDocumentError>
AssignTimeSalesInstrument(WorkspaceState& state, std::string_view id,
                          std::string instrument_id, std::string symbol);
[[nodiscard]] std::expected<void, TimeSalesDocumentError>
ClearTimeSalesInstrument(WorkspaceState& state, std::string_view id);

}  // namespace tradebox::workstation
