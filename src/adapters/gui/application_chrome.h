#pragma once

#include "account_popup.h"
#include "tradebox/workstation/state.h"

#include "imgui.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;

namespace tradebox::gui {

struct ChromeMetrics {
    int title_bar_height = 35;
    int control_width = 46;
    int tool_width = 40;
    int interactive_left_width = 0;
};

struct GuiFonts {
    ImFont* regular = nullptr;
    ImFont* mono = nullptr;
    ImFont* title = nullptr;
    ImFont* icons = nullptr;
    std::array<ImWchar, 11> icon_ranges{
        0xe8b8, 0xe8b8, 0xf20b, 0xf20b, 0xe145, 0xe145,
        0xe8f4, 0xe8f5, 0};
};

struct ChromeActions {
    AccountPopupAction account = AccountPopupAction::None;
    bool new_chart = false;
    bool new_watch_list = false;
    bool new_positions = false;
    bool new_orders = false;
    std::optional<std::string> open_watch_list_id;
    bool new_debug = false;
    bool imgui_demo = false;
    bool settings_changed = false;
};

[[nodiscard]] ChromeActions DrawApplicationChrome(
    SDL_Window* window, ChromeMetrics& metrics, const GuiFonts& fonts,
    std::string_view market_time_text, const core::CoreSnapshot& snapshot,
    std::string_view account_alias,
    bool application_available, AccountPopupState& account_popup,
    const std::vector<application::SavedAccountDescriptor>& saved_accounts,
    std::string_view saved_accounts_error,
    std::string_view current_credential_slot,
    core::AccountEnvironment current_environment,
    std::string_view current_account_id,
    bool& auto_connect,
    const workstation::WorkspaceState& workspace_state,
    workstation::ApplicationSettings& application_settings, bool& done);

}  // namespace tradebox::gui
