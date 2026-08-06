#include "account_popup.h"

#include "gui_controls.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace tradebox::gui {
namespace {

const char* EnvironmentLabel(core::AccountEnvironment environment) {
    return environment == core::AccountEnvironment::Live ? "LIVE" : "PAPER";
}

std::string MaskApiKey(std::string_view key) {
    if (key.empty()) return "Key unavailable";
    if (key.size() <= 4) return std::string(key.size(), '*');
    return std::string(key.substr(0, 2)) + "..." +
           std::string(key.substr(key.size() - 2));
}

std::string AbbreviateAccountId(std::string_view id) {
    if (id.size() <= 8) return std::string(id);
    return std::format("{}...{}", id.substr(0, 4), id.substr(id.size() - 4));
}

std::string FormatMoney(const core::Decimal& value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << value.ToDisplayDouble();
    std::string text = output.str();
    const std::size_t decimal = text.find('.');
    std::size_t first = decimal == std::string::npos ? text.size() : decimal;
    for (std::size_t position = first; position > 3;) {
        position -= 3;
        if (position > 0 && text[position - 1] == '-') continue;
        text.insert(position, 1, ',');
    }
    return "$" + text;
}

void CopyText(std::array<char, 128>& destination, std::string_view value) {
    destination.fill('\0');
    std::copy_n(value.data(),
                std::min(value.size(), destination.size() - 1U),
                destination.data());
}

void ClearCredentials(AccountPopupState& state) {
    state.api_key.fill('\0');
    state.api_secret.fill('\0');
    state.show_api_key = false;
    state.show_api_secret = false;
}

void BeginAddAccount(AccountPopupState& state,
                     core::AccountEnvironment environment) {
    state.account_name.fill('\0');
    ClearCredentials(state);
    state.selected_credential_slot.clear();
    state.environment = environment;
    state.selected_environment = environment;
    state.adding_account = true;
    state.editing_name = false;
    state.remember_credentials = true;
    state.message.clear();
}

AccountPopupAction DrawSavedAccountList(
    const std::vector<application::SavedAccountDescriptor>& saved_accounts,
    std::string_view saved_accounts_error, bool application_available,
    core::AccountEnvironment environment, bool connected,
    std::string_view current_credential_slot,
    std::string_view current_account_id, const core::CoreSnapshot& snapshot,
    bool show_add_account, AccountPopupState& state) {
    AccountPopupAction action = AccountPopupAction::None;
    static_cast<void>(saved_accounts_error);
    static_cast<void>(current_account_id);
    ImGui::SeparatorText(environment == core::AccountEnvironment::Live
                             ? "Live Accounts"
                             : "Paper Accounts");

    for (std::size_t index = 0; index < saved_accounts.size(); ++index) {
        const application::SavedAccountDescriptor& account =
            saved_accounts[index];
        if (account.environment != environment) continue;
        const bool current =
            account.credential_slot == current_credential_slot &&
            account.environment == environment;
        ImGui::PushID(static_cast<int>(index));
        const bool expanded = current && connected;
        const float card_height = expanded ? 235.0f : 72.0f;
        ImGui::Indent(8.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              expanded ? ImVec4(0.12f, 0.15f, 0.20f, 1.0f)
                                       : ImVec4(0.09f, 0.10f, 0.13f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 7.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0f, 8.0f});
        const float card_width =
            std::max(0.0f, ImGui::GetContentRegionAvail().x - 8.0f);
        ImGui::BeginChild("##account_card", {card_width, card_height},
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetWindowFontScale(1.15f);
        ImGui::TextColored({0.92f, 0.92f, 0.96f, 1.0f}, "%s",
                           account.credential_slot.c_str());
        ImGui::SetWindowFontScale(1.0f);
        if (expanded) {
            ImGui::TextColored({0.25f, 0.85f, 0.35f, 1.0f}, "Connected");
        }
#if 0
        if (expanded) {
            ImGui::TextColored({0.25f, 0.85f, 0.35f, 1.0f},
                               "Connected · %s",
                               EnvironmentLabel(account.environment));
        } else {
            ImGui::TextDisabled("%s account",
                                EnvironmentLabel(account.environment));
        }
#endif
#if 0
        ImGui::TextDisabled(
            "%s · %s", EnvironmentLabel(account.environment),
            MaskApiKey(account.api_key_id).c_str());
#endif
        ImGui::BeginDisabled(!application_available || (connected && !current));
        if (current && connected) {
            if (ImGui::Button("Disconnect")) {
                state.selected_credential_slot = account.credential_slot;
                state.selected_environment = account.environment;
                state.request_disconnect_confirmation = true;
                ImGui::CloseCurrentPopup();
            }
        } else {
            if (ImGui::Button("Connect")) {
                state.selected_credential_slot = account.credential_slot;
                state.selected_environment = account.environment;
                CopyText(state.account_name, account.credential_slot);
                state.adding_account = false;
                state.editing_name = false;
                action = AccountPopupAction::Connect;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!application_available);
        if (ImGui::Button("Rename")) {
            state.selected_credential_slot = account.credential_slot;
            state.selected_environment = account.environment;
            state.environment = account.environment;
            CopyText(state.account_name, account.credential_slot);
            state.adding_account = false;
            state.editing_name = true;
            state.message.clear();
            action = AccountPopupAction::EditName;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!application_available);
        if (ImGui::Button("Forget")) {
            state.selected_credential_slot = account.credential_slot;
            state.selected_environment = account.environment;
            state.request_forget_confirmation = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if (current && connected) {
            if (snapshot.account) {
                const auto& info = *snapshot.account;
                ImGui::SetWindowFontScale(0.82f);
                ImGui::TextDisabled("Account ID: %s",
                                    AbbreviateAccountId(info.id).c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\nClick to copy", info.id.c_str());
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        ImGui::SetClipboardText(info.id.c_str());
                }
                ImGui::TextDisabled("Type: %s",
                                    EnvironmentLabel(account.environment));
                ImGui::TextDisabled("Currency: %s", info.currency.c_str());
                ImGui::TextDisabled("Equity: %s", FormatMoney(info.equity).c_str());
                ImGui::TextDisabled("Cash: %s", FormatMoney(info.cash).c_str());
                ImGui::TextDisabled("Buying power: %s",
                            FormatMoney(info.buying_power).c_str());
                ImGui::TextDisabled("Portfolio value: %s",
                            FormatMoney(info.portfolio_value).c_str());
                ImGui::TextDisabled("Shorting: %s", info.shorting_enabled ? "Yes" : "No");
                ImGui::SetWindowFontScale(1.0f);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Unindent(8.0f);
        ImGui::PopID();
    }

    if (show_add_account) {
        const char* label = environment == core::AccountEnvironment::Live
                                ? "Add live account"
                                : "Add paper account";
        if (ImGui::Button(label)) {
            BeginAddAccount(state, environment);
            ImGui::OpenPopup("##add_account_modal");
            ImGui::CloseCurrentPopup();
        }
    }
    return action;
}

}  // namespace

AccountPopupAction DrawAccountPopup(
    ImFont* regular_font, const char* id, ImVec2 anchor,
    const core::CoreSnapshot& snapshot, bool application_available,
    const std::vector<application::SavedAccountDescriptor>& saved_accounts,
    std::string_view saved_accounts_error,
    std::string_view current_credential_slot,
    core::AccountEnvironment current_environment,
    std::string_view current_account_id, ImFont* icon_font,
    AccountPopupState& state) {
    if (state.request_disconnect_confirmation) {
        ImGui::OpenPopup("##disconnect_confirm");
        state.request_disconnect_confirmation = false;
    }
    if (state.request_forget_confirmation) {
        ImGui::OpenPopup("##forget_credentials_confirm");
        state.request_forget_confirmation = false;
    }
    if (state.adding_account) ImGui::OpenPopup("##add_account_modal");

    AccountPopupAction action = AccountPopupAction::None;
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Appearing, {0.0f, 0.0f});
    ImGui::SetNextWindowSizeConstraints({380.0f, 0.0f}, {560.0f, 560.0f});
    if (ImGui::BeginPopup(id)) {
        ImGui::PushFont(regular_font, 18.0f);
        if (!state.initialized) {
            state.selected_credential_slot = std::string(current_credential_slot);
            state.selected_environment = current_environment;
            state.remember_credentials = !saved_accounts.empty();
            state.initialized = true;
        }

        if (state.editing_name) {
            ImGui::TextUnformatted("Edit account name");
            if (!state.message.empty())
                ImGui::TextWrapped("%s", state.message.c_str());
            static_cast<void>(DrawLabeledTextInput(
                "Account name", "##account_name",
                "letters, numbers, - and _", state.account_name.data(),
                state.account_name.size()));
            const bool name_changed = state.account_name[0] != '\0';
            ImGui::BeginDisabled(!application_available || !name_changed);
            if (ImGui::Button("Save name", {-1.0f, 0.0f})) {
                action = AccountPopupAction::SaveName;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Cancel", {-1.0f, 0.0f})) {
                state.editing_name = false;
                ClearCredentials(state);
            }
        } else if (!state.adding_account) {
            if (snapshot.authenticated) {
            state.selected_credential_slot =
                std::string(current_credential_slot);
            state.selected_environment = snapshot.environment;
            }
            const auto live_action = DrawSavedAccountList(
                saved_accounts, saved_accounts_error, application_available,
                core::AccountEnvironment::Live, snapshot.authenticated,
                current_credential_slot, current_account_id, snapshot, true,
                state);
            const auto paper_action = DrawSavedAccountList(
                saved_accounts, saved_accounts_error, application_available,
                core::AccountEnvironment::Paper, snapshot.authenticated,
                current_credential_slot, current_account_id, snapshot, true,
                state);
            action = live_action != AccountPopupAction::None ? live_action
                                                               : paper_action;
            }
        ImGui::PopFont();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowSizeConstraints({360.0f, 0.0f}, {480.0f, 360.0f});
    if (ImGui::BeginPopupModal("##add_account_modal", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushFont(regular_font, 18.0f);
        ImGui::TextUnformatted("Add account");
        if (!state.message.empty())
            ImGui::TextWrapped("%s", state.message.c_str());
        static_cast<void>(DrawLabeledTextInput(
            "Name", "##add_account_name", "account name",
            state.account_name.data(), state.account_name.size()));
        static_cast<void>(DrawLabeledSecretInput(
            "Key", "##add_account_key", "key", state.api_key.data(),
            state.api_key.size(), state.show_api_key, icon_font));
        static_cast<void>(DrawLabeledSecretInput(
            "Secret", "##add_account_secret", "secret",
            state.api_secret.data(), state.api_secret.size(),
            state.show_api_secret, icon_font));
        const bool fields_complete = state.account_name[0] != '\0' &&
                                      state.api_key[0] != '\0' &&
                                      state.api_secret[0] != '\0';
        if (ImGui::Button("CANCEL")) {
            state.adding_account = false;
            state.message.clear();
            ClearCredentials(state);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!application_available || !fields_complete);
        if (ImGui::Button("SAVE")) {
            action = AccountPopupAction::SaveAccount;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
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
