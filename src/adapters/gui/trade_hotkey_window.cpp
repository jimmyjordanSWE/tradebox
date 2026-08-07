#include "trade_hotkey_window.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <string>

namespace tradebox::gui {
namespace {

constexpr std::string_view kTradeHotkeyWindowId = "trade-hotkey.window";

std::pair<workstation::WindowInstanceState&, bool> EnsureWindow(
    workstation::WorkspaceState& state) {
    auto [found, inserted] = state.windows.try_emplace(
        std::string(kTradeHotkeyWindowId), workstation::WindowInstanceState{
            .id = std::string(kTradeHotkeyWindowId),
            .kind = "trade-hotkey",
            .title = "Trade Hotkey",
            .open = true,
            .bounds = {820.0f, 72.0f, 300.0f, 230.0f},
        });
    return {found->second, inserted};
}

}  // namespace

void TradeHotkeyWindowRenderer::Draw(
    ui::Workspace& workspace, workstation::WorkspaceState& state) {
    auto [persisted, inserted] = EnsureWindow(state);
    if (inserted) persistent_changed_ = true;
    if (!persisted.open) return;

    ui::WorkspaceWindow window{
        .title = "Trade Hotkey",
        .id = std::string(kTradeHotkeyWindowId),
        .default_offset = {820.0f, 72.0f},
        .default_size = {300.0f, 230.0f},
        .open = true,
        .flags = ImGuiWindowFlags_NoCollapse,
    };
    workspace.ConstrainNextWindowSize({260.0f, 190.0f});
    if (!workspace.BeginWindow(window)) {
        workspace.EndWindow(window);
        return;
    }

    if (state.selected_symbol.empty()) {
        ImGui::TextUnformatted("No symbol selected");
        ImGui::Separator();
        ImGui::TextDisabled("Select a ticker in a watch list first.");
        ImGui::BeginDisabled();
        static_cast<void>(ImGui::Button("LONG", {118.0f, 34.0f}));
        ImGui::SameLine();
        static_cast<void>(ImGui::Button("SHORT", {118.0f, 34.0f}));
        ImGui::EndDisabled();
        workspace.EndWindow(window);
        return;
    }

    workstation::BracketDraftState& draft =
        state.bracket_drafts[state.selected_symbol];
    ImGui::TextUnformatted(state.selected_symbol.c_str());
    ImGui::Separator();
    auto percentage_control = [&](const char* label, float& value,
                                  float minimum, float maximum) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::InputFloat("##percent", &value, 0.0f, 0.0f, "%.2f%%",
                              ImGuiInputTextFlags_AutoSelectAll)) {
            value = std::clamp(value, minimum, maximum);
            persistent_changed_ = true;
        }
        const bool adjustible = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (adjustible) {
            const float step = ImGui::GetIO().KeyShift ? 0.25f : 0.05f;
            float delta = ImGui::GetIO().MouseWheel * step;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) delta += step;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) delta -= step;
            if (delta != 0.0f) {
                value = std::clamp(value + delta, minimum, maximum);
                persistent_changed_ = true;
            }
        }
        ImGui::PopID();
    };
    if (ImGui::BeginTable("trade_percentages", 2,
                          ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed,
                                92.0f);
        ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthFixed,
                                150.0f);
        percentage_control("Stop Loss", draft.stop_percent, 0.25f, 5.0f);
        percentage_control("Target", draft.target_percent, 0.25f, 5.0f);
        ImGui::EndTable();
    }
    ImGui::Spacing();
    static_cast<void>(ImGui::Button("LONG", {118.0f, 34.0f}));
    ImGui::SameLine();
    static_cast<void>(ImGui::Button("SHORT", {118.0f, 34.0f}));
    ImGui::TextDisabled("Order submission is not wired yet.");

    workspace.EndWindow(window);
}

bool TradeHotkeyWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

}  // namespace tradebox::gui
