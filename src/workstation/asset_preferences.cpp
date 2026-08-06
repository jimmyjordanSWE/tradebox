#include "tradebox/workstation/asset_preferences.h"

#include <algorithm>

namespace tradebox::workstation {

void RecordAssetSelection(WorkspaceState& workspace,
                          std::string_view instrument_id) {
    if (instrument_id.empty()) return;
    auto& history = workspace.asset_selection_history;
    history.erase(std::remove(history.begin(), history.end(), instrument_id),
                  history.end());
    history.insert(history.begin(), std::string(instrument_id));
    if (history.size() > kAssetSelectionHistoryLimit)
        history.resize(kAssetSelectionHistoryLimit);
}

}  // namespace tradebox::workstation
