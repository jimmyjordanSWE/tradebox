#include "watch_list_window.h"

#include "tradebox/workstation/watch_list_documents.h"
#include "tradebox/workstation/asset_preferences.h"

#include "imgui.h"

#include <algorithm>
#include <ranges>

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

}  // namespace

void WatchListWindowRenderer::AppendSnapshotQuery(
    workstation::WorkspaceState& state,
    application::UiSnapshotQuery& query) const {
    for (const workstation::WatchListDocumentState& document :
         state.watch_lists) {
        const auto window = state.windows.find(document.id);
        if (window == state.windows.end() || !window->second.open) continue;
        for (const workstation::WatchListRowState& row : document.rows) {
            if (!row.instrument_id.empty() && !row.symbol.empty())
                query.market_symbols.push_back(row.symbol);
            else if (!row.ticker_input.empty())
                query.asset_searches.push_back(row.ticker_input);
        }
    }
    query.asset_preferred_instrument_ids = state.asset_selection_history;
    if (!query.asset_searches.empty())
        query.asset_limit = std::max<std::size_t>(query.asset_limit, 8U);
}

void WatchListWindowRenderer::Draw(
    ui::Workspace& workspace, workstation::WorkspaceState& state,
    const application::ApplicationUiSnapshot& snapshot) {
    for (workstation::WatchListDocumentState& document : state.watch_lists) {
        const auto window_state = state.windows.find(document.id);
        if (window_state == state.windows.end() || !window_state->second.open)
            continue;

        ui::WorkspaceWindow window{
            .title = document.name,
            .id = document.id,
            .default_offset = {72.0f, 72.0f},
            .default_size = {720.0f, 480.0f},
            .open = true,
        };
        workspace.ConstrainNextWindowSize({420.0f, 220.0f});
        if (!workspace.BeginWindow(window)) {
            workspace.EndWindow(window);
            continue;
        }

        ImGui::PushID(document.id.c_str());
        ImGui::TextUnformatted("Symbol");
        ImGui::Separator();
        for (workstation::WatchListRowState& row : document.rows) {
            ImGui::PushID(row.id.c_str());
            RowInteraction& interaction = interactions_[row.id];
            if (!interaction.initialized) {
                const auto count = std::min(
                    row.ticker_input.size(), interaction.ticker.size() - 1U);
                std::copy_n(row.ticker_input.data(), count,
                            interaction.ticker.data());
                interaction.ticker[count] = '\0';
                interaction.initialized = true;
            }
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::InputText("##ticker", interaction.ticker.data(),
                                interaction.ticker.size(),
                                ImGuiInputTextFlags_CharsUppercase)) {
                row.ticker_input = interaction.ticker.data();
                row.instrument_id.clear();
                row.symbol.clear();
                persistent_changed_ = true;
            }
            if (!row.instrument_id.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", row.symbol.c_str());
            }

            if (row.instrument_id.empty() && !row.ticker_input.empty()) {
                const auto* matches =
                    MatchesFor(snapshot, row.ticker_input);
                if (matches != nullptr) {
                    for (const core::TradableAsset& asset : matches->matches) {
                        const std::string label =
                            asset.symbol + " - " + asset.name;
                        if (!ImGui::Selectable(label.c_str(), false))
                            continue;
                        const auto assigned =
                            workstation::AssignWatchListRowAsset(
                                state, document.id, row.id,
                                asset.instrument_id, asset.symbol);
                        if (assigned) {
                            workstation::RecordAssetSelection(
                                state, asset.instrument_id);
                            const auto count = std::min(
                                asset.symbol.size(), interaction.ticker.size() - 1U);
                            std::copy_n(asset.symbol.data(), count,
                                        interaction.ticker.data());
                            interaction.ticker[count] = '\0';
                            persistent_changed_ = true;
                        }
                    }
                }
            }
            ImGui::PopID();
        }
        if (ImGui::Button("+ Add row")) {
            if (workstation::AddWatchListRow(state, document.id))
                persistent_changed_ = true;
        }
        ImGui::PopID();
        workspace.EndWindow(window);
    }
}

bool WatchListWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

}  // namespace tradebox::gui
