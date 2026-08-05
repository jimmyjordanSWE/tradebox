#pragma once

#include "tradebox/core/types.h"

#include "imgui.h"

#include <array>
#include <string>

namespace tradebox::gui {

enum class AccountPopupAction {
    None,
    Connect,
    Disconnect,
    ForgetCredentials,
};

struct AccountPopupState {
    std::array<char, 256> api_key{};
    std::array<char, 256> api_secret{};
    core::AccountEnvironment environment = core::AccountEnvironment::Paper;
    bool remember_credentials = false;
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
    bool saved_credentials_available, ImFont* icon_font,
    AccountPopupState& state);

}  // namespace tradebox::gui
