#pragma once

#include "tradebox/application/ui_snapshot.h"
#include "tradebox/core/types.h"

#include "imgui.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace tradebox::gui {

enum class AccountPopupAction {
    None,
    Connect,
    SaveAccount,
    EditName,
    SaveName,
    Disconnect,
    ForgetCredentials,
};

struct AccountPopupState {
    std::array<char, 128> account_name{};
    std::array<char, 256> api_key{};
    std::array<char, 256> api_secret{};
    std::string selected_credential_slot;
    core::AccountEnvironment selected_environment =
        core::AccountEnvironment::Paper;
    core::AccountEnvironment environment = core::AccountEnvironment::Paper;
    bool remember_credentials = false;
    bool adding_account = false;
    bool editing_name = false;
    bool initialized = false;
    bool show_api_key = false;
    bool show_api_secret = false;
    bool live_trading_confirmed = false;
    bool request_disconnect_confirmation = false;
    bool request_forget_confirmation = false;
    std::string message;
};

[[nodiscard]] AccountPopupAction DrawAccountPopup(
    ImFont* regular_font, const char* id, ImVec2 anchor,
    const core::CoreSnapshot& snapshot, bool application_available,
    const std::vector<application::SavedAccountDescriptor>& saved_accounts,
    std::string_view saved_accounts_error,
    std::string_view current_credential_slot,
    core::AccountEnvironment current_environment,
    std::string_view current_account_id, ImFont* icon_font,
    AccountPopupState& state);

}  // namespace tradebox::gui
