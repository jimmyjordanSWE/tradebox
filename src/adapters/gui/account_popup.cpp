#include "account_popup.h"

#include "gui_controls.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>

namespace tradebox::gui {
namespace {

std::string AbbreviateAccountId(std::string_view id) {
    if (id.size() <= 8) return std::string(id);
    return std::format("{}...{}", id.substr(0, 4), id.substr(id.size() - 4));
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
    static_cast<void>(show_add_account);

    // The first item of every environment section is the add-account entry,
    // so an empty section (e.g. no live accounts saved) still offers it.
    if (show_add_account) {
        const char* label = environment == core::AccountEnvironment::Live
                                ? "Add live account..."
                                : "Add paper account...";
        if (ImGui::MenuItem(label)) {
            BeginAddAccount(state, environment);
            ImGui::OpenPopup("##add_account_modal");
            ImGui::CloseCurrentPopup();
        }
    }

    const bool connecting =
        snapshot.safety_status == core::SafetyStatus::Connecting;
    const ImVec4 kConnectedGreen{0.25f, 0.85f, 0.35f, 1.0f};
    const ImVec4 kConnectingAmber{0.95f, 0.75f, 0.25f, 1.0f};
    const auto money = [](const core::Decimal& value) {
        return std::format("${:.2f}", value.ToDisplayDouble());
    };
    for (std::size_t index = 0; index < saved_accounts.size(); ++index) {
        const application::SavedAccountDescriptor& account =
            saved_accounts[index];
        if (account.environment != environment) continue;
        const bool current =
            account.credential_slot == current_credential_slot &&
            account.environment == environment;
        const bool is_connected = current && connected;
        // Account data is REST truth fetched from /v2/account and is
        // independent of the trade_updates stream, so a routine stream flap
        // (idle timeout, transient network) must not hide it. Whenever account
        // data is present for the current account, show it. "Connected" is
        // only truthful when the stream is also authenticated; while the
        // stream is reconnecting the data stays visible under an amber
        // "Reconnecting" label.
        const bool has_account = snapshot.account.has_value();
        const bool show_account = current && has_account;
        const bool show_connected = is_connected && has_account;
        // The account-stream loop labels the flap state; only a Stale safety
        // status means the stream dropped after a live session. Otherwise a
        // data-bearing row without an authenticated stream is still in the
        // initial connection window.
        const bool stream_stale =
            snapshot.safety_status == core::SafetyStatus::Stale;

        ImGui::PushID(static_cast<int>(index));
        // Every saved account is an expandable submenu. The connected one
        // reads "name · Connected" in the list so it is identifiable at a
        // glance even before expanding; while the stream is reconnecting it
        // reads "name · Reconnecting" in amber with the data still visible.
        std::string account_label = account.credential_slot;
        if (show_account) {
            const char* suffix = show_connected
                                     ? " · Connected"
                                     : (stream_stale ? " · Reconnecting"
                                                     : " · Connecting");
            account_label += suffix;
        }
        if (show_account)
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                show_connected ? kConnectedGreen : kConnectingAmber);
        const bool open = ImGui::BeginMenu(account_label.c_str());
        if (show_account) ImGui::PopStyleColor();
        if (open) {
            if (show_account) {
                if (show_connected)
                    ImGui::TextColored(kConnectedGreen, "Connected");
                else
                    ImGui::TextColored(
                        kConnectingAmber,
                        stream_stale ? "Reconnecting..." : "Connecting...");
                if (snapshot.account) {
                    const auto& info = *snapshot.account;
                    ImGui::Indent(22.0f);
                    ImGui::TextDisabled("Account ID: %s",
                                        AbbreviateAccountId(info.id).c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s\nClick to copy",
                                          info.id.c_str());
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                            ImGui::SetClipboardText(info.id.c_str());
                    }
                    ImGui::TextDisabled("Account #: %s",
                                        info.account_number.c_str());
                    ImGui::TextDisabled(
                        "Type: %s",
                        account.environment == core::AccountEnvironment::Live
                            ? "Live"
                            : "Paper");
                    ImGui::TextDisabled("Currency: %s",
                                        info.currency.c_str());
                    ImGui::TextDisabled("Equity: %s",
                                        money(info.equity).c_str());
                    ImGui::TextDisabled("Cash: %s",
                                        money(info.cash).c_str());
                    ImGui::TextDisabled(
                        "Buying power: %s", money(info.buying_power).c_str());
                    ImGui::TextDisabled(
                        "Portfolio value: %s",
                        money(info.portfolio_value).c_str());
                    ImGui::TextDisabled(
                        "Long market value: %s",
                        money(info.long_market_value).c_str());
                    ImGui::TextDisabled(
                        "Short market value: %s",
                        money(info.short_market_value).c_str());
                    ImGui::TextDisabled(
                        "Shorting: %s", info.shorting_enabled ? "Yes" : "No");
                    const std::int64_t now_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now()
                                .time_since_epoch())
                            .count();
                    const std::int64_t age_s = std::max<std::int64_t>(
                        0, (now_ms - info.received_at_ms) / 1000);
                    ImGui::TextDisabled(
                        "Updated: %s",
                        age_s < 60
                            ? std::format("{} s ago", age_s).c_str()
                            : std::format("{} min ago", age_s / 60).c_str());
                    ImGui::Unindent(22.0f);
                } else {
                    ImGui::TextDisabled("Account data not loaded yet");
                }
                ImGui::Separator();
                ImGui::BeginDisabled(!application_available);
                if (ImGui::Button("Disconnect", {-1.0f, 0.0f})) {
                    state.selected_credential_slot = account.credential_slot;
                    state.selected_environment = account.environment;
                    state.request_disconnect_confirmation = true;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Rename")) {
                    state.selected_credential_slot = account.credential_slot;
                    state.selected_environment = account.environment;
                    state.environment = account.environment;
                    CopyText(state.account_name, account.credential_slot);
                    state.adding_account = false;
                    state.editing_name = true;
                    state.message.clear();
                    action = AccountPopupAction::EditName;
                }
                if (ImGui::MenuItem("Forget")) {
                    state.selected_credential_slot = account.credential_slot;
                    state.selected_environment = account.environment;
                    state.request_forget_confirmation = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
            } else {
                if (current && connecting)
                    ImGui::TextColored(kConnectingAmber, "Connecting...");
                ImGui::BeginDisabled(!application_available);
                if (ImGui::MenuItem("Connect")) {
                    state.selected_credential_slot = account.credential_slot;
                    state.selected_environment = account.environment;
                    CopyText(state.account_name, account.credential_slot);
                    state.adding_account = false;
                    state.editing_name = false;
                    action = AccountPopupAction::Connect;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SetItemTooltip(
                    "Connecting switches to this account (one connection at a "
                    "time)");
                if (ImGui::MenuItem("Rename")) {
                    state.selected_credential_slot = account.credential_slot;
                    state.selected_environment = account.environment;
                    state.environment = account.environment;
                    CopyText(state.account_name, account.credential_slot);
                    state.adding_account = false;
                    state.editing_name = true;
                    state.message.clear();
                    action = AccountPopupAction::EditName;
                }
                if (ImGui::MenuItem("Forget")) {
                    state.selected_credential_slot = account.credential_slot;
                    state.selected_environment = account.environment;
                    state.request_forget_confirmation = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
            }
            ImGui::EndMenu();
        }
        ImGui::PopID();
    }
    return action;
}

}  // namespace

AccountPopupAction DrawAccountMenu(
    ImFont* regular_font, const core::CoreSnapshot& snapshot,
    bool application_available,
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
    if (ImGui::BeginMenu("Accounts")) {
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
            ImGui::SeparatorText("Paper");
            const auto paper_action = DrawSavedAccountList(
                saved_accounts, saved_accounts_error, application_available,
                core::AccountEnvironment::Paper, snapshot.authenticated,
                current_credential_slot, current_account_id, snapshot, true,
                state);
            ImGui::SeparatorText("Live");
            const auto live_action = DrawSavedAccountList(
                saved_accounts, saved_accounts_error, application_available,
                core::AccountEnvironment::Live, snapshot.authenticated,
                current_credential_slot, current_account_id, snapshot, true,
                state);
            action = live_action != AccountPopupAction::None ? live_action
                                                               : paper_action;
            }
        ImGui::PopFont();
        ImGui::EndMenu();
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
