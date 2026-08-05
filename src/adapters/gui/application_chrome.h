#pragma once

#include "account_popup.h"
#include "tradebox/workstation/state.h"

#include "imgui.h"

#include <array>
#include <string_view>

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
    ImFont* title = nullptr;
    ImFont* icons = nullptr;
    std::array<ImWchar, 11> icon_ranges{
        0xe8b8, 0xe8b8, 0xf20b, 0xf20b, 0xe145, 0xe145,
        0xe8f4, 0xe8f5, 0};
};

struct ChromeActions {
    AccountPopupAction account = AccountPopupAction::None;
    bool new_chart = false;
    bool imgui_demo = false;
    bool settings_changed = false;
};

[[nodiscard]] ChromeActions DrawApplicationChrome(
    SDL_Window* window, ChromeMetrics& metrics, const GuiFonts& fonts,
    std::string_view market_time_text, const core::CoreSnapshot& snapshot,
    bool application_available, AccountPopupState& account_popup,
    bool saved_credentials_available,
    workstation::ApplicationSettings& application_settings, bool& done);

}  // namespace tradebox::gui
