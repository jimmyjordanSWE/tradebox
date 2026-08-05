#include "account_popup.h"

#include "gui_controls.h"

#include <algorithm>

namespace tradebox::gui {

AccountPopupAction DrawAccountPopup(
    ImFont* regular_font, const char* id, ImVec2 anchor,
    const core::CoreSnapshot& snapshot, bool application_available,
    bool saved_credentials_available, ImFont* icon_font,
    AccountPopupState& state) {
    if (state.request_disconnect_confirmation) {
        ImGui::OpenPopup("##disconnect_confirm");
        state.request_disconnect_confirmation = false;
    }
    if (state.request_forget_confirmation) {
        ImGui::OpenPopup("##forget_credentials_confirm");
        state.request_forget_confirmation = false;
    }

    AccountPopupAction action = AccountPopupAction::None;
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Appearing, {0.0f, 0.0f});
    ImGui::SetNextWindowSizeConstraints({360.0f, 0.0f}, {480.0f, 420.0f});
    if (ImGui::BeginPopup(id)) {
        ImGui::PushFont(regular_font, 18.0f);
        if (!state.initialized) {
            state.remember_credentials = saved_credentials_available;
            state.initialized = true;
        }
        if (snapshot.authenticated) {
            if (ImGui::Button("Disconnect", {-1.0f, 0.0f})) {
                state.request_disconnect_confirmation = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (snapshot.account) {
                const auto& account = *snapshot.account;
                ImGui::Text("Account ID: %s", account.id.c_str());
                ImGui::Text("Status: %s", account.status.c_str());
                ImGui::Text("Currency: %s", account.currency.c_str());
                ImGui::Text("Equity: %s", account.equity.ToString().c_str());
                ImGui::Text("Cash: %s", account.cash.ToString().c_str());
                ImGui::Text("Buying power: %s",
                            account.buying_power.ToString().c_str());
                ImGui::Text("Portfolio value: %s",
                            account.portfolio_value.ToString().c_str());
                ImGui::Text("Shorting: %s",
                            account.shorting_enabled ? "enabled" : "disabled");
            } else {
                ImGui::TextUnformatted("Loading account information...");
            }
            ImGui::Separator();
            ImGui::BeginDisabled(!saved_credentials_available);
            if (ImGui::Button("Forget credentials", {-1.0f, 0.0f})) {
                state.request_forget_confirmation = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
        } else {
            const bool fields_complete = state.api_key[0] != '\0' &&
                                          state.api_secret[0] != '\0';
            if (!state.message.empty())
                ImGui::TextWrapped("%s", state.message.c_str());
            static_cast<void>(DrawLabeledSecretInput(
                "", "##api_key", "key", state.api_key.data(),
                state.api_key.size(), state.show_api_key, icon_font));
            static_cast<void>(DrawLabeledSecretInput(
                "", "##api_secret", "secret",
                state.api_secret.data(), state.api_secret.size(),
                state.show_api_secret, icon_font));
            const bool can_connect = application_available &&
                                     (saved_credentials_available ||
                                      fields_complete);
            ImGui::BeginDisabled(!can_connect);
            if (ImGui::Button("Connect", {-1.0f, 0.0f})) {
                action = AccountPopupAction::Connect;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (saved_credentials_available) {
                ImGui::TextDisabled("Credentials are saved on this computer");
            }
            ImGui::Checkbox("Remember this computer", &state.remember_credentials);
            ImGui::BeginDisabled(!saved_credentials_available);
            if (ImGui::Button("Forget credentials", {-1.0f, 0.0f})) {
                state.request_forget_confirmation = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
        }
        ImGui::PopFont();
        ImGui::EndPopup();
    }

    if (snapshot.authenticated &&
        snapshot.environment == core::AccountEnvironment::Live &&
        !state.live_trading_confirmed) {
        ImGui::OpenPopup("##live_trading_confirmation");
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos,
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size,
                             ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(
            "##live_trading_confirmation", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(regular_font, 18.0f);
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float content_width = std::min(520.0f, available.x - 48.0f);
        ImGui::SetCursorPos({(available.x - content_width) * 0.5f,
                             available.y * 0.36f});
        ImGui::BeginGroup();
        ImGui::TextColored({0.95f, 0.62f, 0.16f, 1.0f},
                           "LIVE ACCOUNT CONNECTED");
        ImGui::TextWrapped(
            "You connected a live Alpaca account. Real money is used for "
            "trading actions.");
        ImGui::Spacing();
        if (ImGui::Button("Yes, I know I'm trading real money",
                          {content_width, 0.0f})) {
            state.live_trading_confirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndGroup();
        ImGui::PopFont();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowSizeConstraints({320.0f, 0.0f}, {440.0f, 220.0f});
    if (ImGui::BeginPopupModal("##disconnect_confirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushFont(regular_font, 18.0f);
        ImGui::TextUnformatted("Disconnect account?");
        ImGui::TextWrapped("Are you sure you want to disconnect from Alpaca?");
        if (ImGui::Button("Disconnect")) {
            action = AccountPopupAction::Disconnect;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSizeConstraints({320.0f, 0.0f}, {440.0f, 220.0f});
    if (ImGui::BeginPopupModal("##forget_credentials_confirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushFont(regular_font, 18.0f);
        ImGui::TextUnformatted("Forget saved credentials?");
        ImGui::TextWrapped(
            "The saved key and secret will be removed from Windows "
            "Credential Manager.");
        if (ImGui::Button("Forget credentials")) {
            action = AccountPopupAction::ForgetCredentials;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    return action;
}

}  // namespace tradebox::gui
