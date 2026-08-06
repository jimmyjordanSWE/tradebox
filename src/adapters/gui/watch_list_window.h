#pragma once

#include "tradebox/application/trading_application.h"
#include "tradebox/ui/workspace.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tradebox::gui {

class WatchListWindowRenderer final {
public:
    void StartNewDraft(workstation::WorkspaceState& state);
    void OpenSavedDocument(workstation::WorkspaceState& state,
                           std::string_view document_id);
    void AppendSnapshotQuery(
        workstation::WorkspaceState& state,
        application::UiSnapshotQuery& query);
    void RequestMissingHistory(
        application::TradingApplication& application,
        const application::ApplicationUiSnapshot& snapshot);
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              const application::ApplicationUiSnapshot& snapshot,
              ImFont* icons);
    [[nodiscard]] bool ConsumePersistentChanges();

private:
    [[nodiscard]] workstation::WatchListDocumentState* ActiveDocument(
        workstation::WorkspaceState& state);
    void EnsureSession(workstation::WorkspaceState& state);
    void SaveCurrentDraft(workstation::WorkspaceState& state);
    void DeleteSavedDocument(workstation::WorkspaceState& state,
                             std::string_view document_id);
    void CommitName(workstation::WorkspaceState& state);

    struct RowInteraction {
        std::array<char, 64> ticker{};
        bool initialized = false;
        int highlighted_match = -1;
        int navigation_direction = 0;
    };

    std::unordered_map<std::string, RowInteraction> interactions_;
    std::unordered_map<std::string, application::UiWatchListQuery>
        queries_;
    std::unordered_map<std::string, core::BarRange> requested_history_;
    std::optional<workstation::WatchListDocumentState> draft_;
    std::array<char, 128> name_input_{};
    std::string focus_row_id_;
    bool editing_name_ = false;
    bool focus_name_input_ = false;
    std::string message_;
    bool persistent_changed_ = false;
};

}  // namespace tradebox::gui
