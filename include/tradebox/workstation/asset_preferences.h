#pragma once

#include "tradebox/workstation/state.h"

#include <string_view>

namespace tradebox::workstation {

inline constexpr std::size_t kAssetSelectionHistoryLimit = 256;

void RecordAssetSelection(WorkspaceState& workspace,
                          std::string_view instrument_id);

}  // namespace tradebox::workstation
