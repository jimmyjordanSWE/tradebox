#include "trade_hotkey_window.h"

#include "tradebox/workstation/trade_hotkey.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace tradebox::gui {
namespace {

std::optional<core::Decimal> DecimalFromPercent(float value) {
    auto parsed = core::Decimal::Parse(std::format("{:.2f}", value));
    if (!parsed) return std::nullopt;
    return *parsed;
}

void DrawCommitment(
    const char* label,
    const std::optional<application::HotkeyBracketPreview>& preview,
    const std::string& error) {
    if (!preview) {
        ImGui::Text("%s: unavailable", label);
        if (!error.empty()) ImGui::SetItemTooltip("%s", error.c_str());
        return;
    }
    ImGui::Text("%s: %s", label,
                preview->estimated_entry_notional.ToString().c_str());
}

}  // namespace

void TradeHotkeyWindowRenderer::Draw(
    ui::Workspace& workspace, workstation::WorkspaceState& state,
    workstation::ApplicationSettings& settings) {
    if (pending_result_ &&
        pending_result_->wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        result_ = pending_result_->get();
        pending_result_.reset();
    }
    const auto persisted = state.windows.find(
        std::string(workstation::kTradeHotkeyWindowId));
    if (persisted == state.windows.end()) {
        if (!workstation::OpenTradeHotkeyWindow(state)) return;
        persistent_changed_ = true;
    } else if (!persisted->second.open) {
        return;
    }

    ui::WorkspaceWindow window{
        .title = "Trade Hotkey",
        .id = std::string(workstation::kTradeHotkeyWindowId),
        .default_offset = {820, 72},
        .default_size = {300, 250},
        .open = true,
        .flags = ImGuiWindowFlags_NoCollapse};
    workspace.ConstrainNextWindowSize({260, 220});
    if (!workspace.BeginWindow(window)) {
        workspace.EndWindow(window);
        return;
    }
    if (state.selected_symbol.empty()) {
        ImGui::TextUnformatted("No symbol selected");
        workspace.EndWindow(window);
        return;
    }

    auto& draft = state.bracket_drafts[state.selected_symbol];
    ImGui::TextUnformatted(state.selected_symbol.c_str());
    ImGui::Separator();
    const auto input = [&](const char* name, float& value, float minimum,
                           float maximum) {
        if (ImGui::InputFloat(name, &value, 0, 0, "%.2f%%",
                              ImGuiInputTextFlags_AutoSelectAll)) {
            value = std::clamp(value, minimum, maximum);
            persistent_changed_ = true;
            long_preview_.reset();
            short_preview_.reset();
        }
    };
    input("Stop Loss", draft.stop_percent, .01f, 100.f);
    input("Target", draft.target_percent, .01f, 100.f);
    if (ImGui::Checkbox("GTC", &draft.gtc)) {
        persistent_changed_ = true;
        long_preview_.reset();
        short_preview_.reset();
    }

    const auto build_intent = [&](application::HotkeyOrderSide side)
        -> std::optional<application::HotkeyBracketIntent> {
        const auto risk =
            DecimalFromPercent(settings.account_risk_per_trade_percent);
        const auto stop = DecimalFromPercent(draft.stop_percent);
        const auto target = DecimalFromPercent(draft.target_percent);
        if (!risk || !stop || !target) {
            return std::nullopt;
        }
        const float maximum = side == application::HotkeyOrderSide::Long
                                  ? settings.max_long_buying_power_percent
                                  : settings.max_short_buying_power_percent;
        const auto buying_power = DecimalFromPercent(maximum);
        if (!buying_power) {
            return std::nullopt;
        }
        return application::HotkeyBracketIntent{
            state.selected_symbol, side, *risk, *target, *stop, *buying_power,
            draft.gtc};
    };

    const auto long_intent = build_intent(application::HotkeyOrderSide::Long);
    const auto short_intent = build_intent(application::HotkeyOrderSide::Short);
    if (long_intent && short_intent)
        preview_request_ = HotkeyPreviewRequest{*long_intent, *short_intent};

    if (ImGui::Button("LONG", {118, 34}) && long_intent) {
        active_intent_ = *long_intent;
        submission_request_ = *long_intent;
        result_.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("SHORT", {118, 34}) && short_intent) {
        active_intent_ = *short_intent;
        submission_request_ = *short_intent;
        result_.reset();
    }

    if (long_preview_ || short_preview_) {
        ImGui::Separator();
        ImGui::TextUnformatted("Live calculation");
        DrawCommitment("Long commitment", long_preview_, long_preview_error_);
        DrawCommitment("Short commitment", short_preview_, short_preview_error_);
    }
    workspace.EndWindow(window);
}

std::optional<HotkeyPreviewRequest>
TradeHotkeyWindowRenderer::ConsumePreviewRequest() {
    return std::exchange(preview_request_, std::nullopt);
}

std::optional<application::HotkeyBracketIntent>
TradeHotkeyWindowRenderer::ConsumeSubmissionRequest() {
    return std::exchange(submission_request_, std::nullopt);
}

void TradeHotkeyWindowRenderer::SetPreviews(
    std::expected<application::HotkeyBracketPreview, std::string> long_preview,
    std::expected<application::HotkeyBracketPreview, std::string> short_preview) {
    if (long_preview) {
        long_preview_ = std::move(*long_preview);
        long_preview_error_.clear();
    } else {
        long_preview_.reset();
        long_preview_error_ = long_preview.error();
    }
    if (short_preview) {
        short_preview_ = std::move(*short_preview);
        short_preview_error_.clear();
    } else {
        short_preview_.reset();
        short_preview_error_ = short_preview.error();
    }
}

void TradeHotkeyWindowRenderer::SetSubmissionResult(
    std::future<core::OrderCommandResult> result) {
    pending_result_ = std::move(result);
}

bool TradeHotkeyWindowRenderer::ConsumePersistentChanges() {
    const bool changed = persistent_changed_;
    persistent_changed_ = false;
    return changed;
}

}  // namespace tradebox::gui
