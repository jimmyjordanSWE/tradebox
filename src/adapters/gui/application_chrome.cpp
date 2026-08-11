#include "application_chrome.h"

#include "gui_controls.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace tradebox::gui {
namespace {

struct CreationActions {
    bool new_chart = false;
    bool new_watch_list = false;
    bool new_positions = false;
    bool new_orders = false;
    bool new_trade_hotkey = false;
    bool new_events = false;
    bool new_time_sales = false;
    std::optional<std::string> open_watch_list_id;
};

void DrawBuyingPowerMeter(const core::AccountState& account) {
    const auto multiplier = core::Decimal::Parse(account.multiplier);
    const core::Decimal buying_power_capacity =
        multiplier && *multiplier > core::Decimal::Zero()
            ? account.equity * *multiplier
            : account.equity;
    std::optional<core::Decimal> available_ratio;
    if (buying_power_capacity > core::Decimal::Zero()) {
        const auto divided = account.buying_power.Divide(buying_power_capacity, 6);
        if (divided) available_ratio = *divided;
    }
    const float available_percent = available_ratio
                                        ? std::max(
                                              static_cast<float>(
                                                  available_ratio->ToDisplayDouble()),
                                              0.0f)
                                        : 0.0f;
    const std::string summary = std::format(
        "BP ${:.0f} left  |  {:.0f}% available",
        account.buying_power.ToDisplayDouble(), available_percent * 100.0f);
    ImGui::TextDisabled("%s", summary.c_str());
    const std::string tooltip = std::format(
        "Buying power left: ${:.2f}\nBuying-power capacity: ${:.2f}\nAvailable: {:.1f}%",
        account.buying_power.ToDisplayDouble(),
        buying_power_capacity.ToDisplayDouble(), available_percent * 100.0f);
    ImGui::SetItemTooltip("%s", tooltip.c_str());
}

CreationActions DrawCreationMenu(
    ImFont* regular_font, const workstation::WorkspaceState& /*state*/) {
    CreationActions actions;
    if (!ImGui::BeginMenu("Add Window")) return actions;

    ImGui::PushFont(regular_font, 18.0f);
    if (ImGui::MenuItem("Chart", "")) actions.new_chart = true;
    if (ImGui::MenuItem("Watchlist", "")) actions.new_watch_list = true;
    if (ImGui::MenuItem("Positions", "")) actions.new_positions = true;
    if (ImGui::MenuItem("Orders", "")) actions.new_orders = true;
    if (ImGui::MenuItem("Trade Hotkey", "")) actions.new_trade_hotkey = true;
    if (ImGui::MenuItem("Events", "")) actions.new_events = true;
    if (ImGui::MenuItem("Time & Sales", "")) actions.new_time_sales = true;
    ImGui::PopFont();
    ImGui::EndMenu();
    return actions;
}

void DrawSettingsMenu(ImFont* regular_font,
                      workstation::ApplicationSettings& settings,
                      bool& auto_connect, bool& changed,
                      bool& return_all_windows_to_workspace) {
    if (!ImGui::BeginMenu("Settings")) return;

    ImGui::PushFont(regular_font, 18.0f);
    if (ImGui::Checkbox("Auto connect last used account", &auto_connect))
        changed = true;
    bool vsync = settings.vsync_requested;
    if (ImGui::Checkbox("VSync", &vsync)) {
        settings.vsync_requested = vsync;
        changed = true;
    }
    ImGui::SetItemTooltip("Synchronizes rendering with the display refresh.");
    float interface_scale_percent = settings.ui_scale * 100.0f;
    if (ImGui::SliderFloat("Interface scale", &interface_scale_percent,
                           70.0f, 200.0f, "%.0f%%")) {
        settings.ui_scale = std::clamp(interface_scale_percent / 100.0f,
                                       0.70f, 2.00f);
        changed = true;
    }
    ImGui::SetItemTooltip(
        "Scales text, controls, and workspace windows. Ctrl+Plus, Ctrl+Minus, "
        "and Ctrl+0 also adjust this setting.");
    int window_snap_pixels = settings.window_snap_pixels;
    if (ImGui::SliderInt("Window snap", &window_snap_pixels, 1, 100,
                         "%d px")) {
        settings.window_snap_pixels = std::clamp(window_snap_pixels, 1, 1000);
        changed = true;
    }
    ImGui::SetItemTooltip(
        "When a window is dropped, it snaps to this grid or to an exact "
        "workspace edge.");
    if (ImGui::Button("Return all windows to workspace"))
        return_all_windows_to_workspace = true;
    ImGui::SetItemTooltip(
        "Fits windows to this workspace and returns off-screen windows to "
        "the top-left corner.");
    int maximum_frame_rate = settings.maximum_frame_rate;
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::InputInt("Max framerate", &maximum_frame_rate, 1, 10,
                        ImGuiInputTextFlags_CharsDecimal)) {
        settings.maximum_frame_rate =
            std::clamp(maximum_frame_rate, 30, 240);
        changed = true;
    }
    ImGui::SeparatorText("Trade Hotkey");
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::InputFloat("Account risk per trade",
                          &settings.account_risk_per_trade_percent,
                          0.05f, 0.25f,
                          "%.2f%%")) {
        settings.account_risk_per_trade_percent = std::clamp(
            settings.account_risk_per_trade_percent, 0.01f, 100.0f);
        changed = true;
    }
    if (ImGui::InputFloat("Max Long buying power",
                          &settings.max_long_buying_power_percent,
                          1.0f, 5.0f, "%.1f%%")) {
        settings.max_long_buying_power_percent = std::clamp(
            settings.max_long_buying_power_percent, 0.01f, 100.0f);
        changed = true;
    }
    if (ImGui::InputFloat("Max Short buying power",
                          &settings.max_short_buying_power_percent,
                          1.0f, 5.0f, "%.1f%%")) {
        settings.max_short_buying_power_percent = std::clamp(
            settings.max_short_buying_power_percent, 0.01f, 100.0f);
        changed = true;
    }
    ImGui::SeparatorText("Watch List Percentage Colours");
    const auto edit_color = [&](const char* label, std::uint32_t& packed) {
        ImVec4 color{
            static_cast<float>((packed >> 24U) & 0xffU) / 255.0f,
            static_cast<float>((packed >> 16U) & 0xffU) / 255.0f,
            static_cast<float>((packed >> 8U) & 0xffU) / 255.0f,
            static_cast<float>(packed & 0xffU) / 255.0f};
        if (ImGui::ColorEdit4(label, &color.x,
                              ImGuiColorEditFlags_AlphaBar)) {
            packed = (static_cast<std::uint32_t>(color.x * 255.0f + 0.5f) << 24U) |
                     (static_cast<std::uint32_t>(color.y * 255.0f + 0.5f) << 16U) |
                     (static_cast<std::uint32_t>(color.z * 255.0f + 0.5f) << 8U) |
                     static_cast<std::uint32_t>(color.w * 255.0f + 0.5f);
            changed = true;
        }
    };
    edit_color("Above +3%", settings.watch_list_strong_green_rgba);
    edit_color("0% to +3%", settings.watch_list_light_green_rgba);
    edit_color("0% to -3%", settings.watch_list_light_red_rgba);
    edit_color("Below -3%", settings.watch_list_strong_red_rgba);
    ImGui::PopFont();
    ImGui::EndMenu();
}

}  // namespace

ChromeActions DrawApplicationChrome(
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
    workstation::ApplicationSettings& application_settings, bool& done) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    // The chrome window is sized to exactly the menu bar height; sizing it to
    // GetFrameHeightWithSpacing() would leave a visible gap between the menu
    // bar and the border line below it.
    metrics.title_bar_height = static_cast<int>(ImGui::GetFrameHeight());
    metrics.control_width = 46;
    const float row_height = static_cast<float>(metrics.title_bar_height);
    viewport->WorkPos = {viewport->Pos.x, viewport->Pos.y + row_height};
    viewport->WorkSize = {
        viewport->Size.x, std::max(0.0f, viewport->Size.y - row_height)};
    const float caption_controls_width =
        static_cast<float>(metrics.control_width * 3);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_MenuBar;
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize({viewport->Size.x, row_height}, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 0.0f));
    ChromeActions actions;
    if (ImGui::Begin("##tradebox_chrome", nullptr, flags)) {
        const ImVec2 window_size = ImGui::GetWindowSize();

        ImGui::BeginMenuBar();

        actions.account = DrawAccountMenu(
            fonts.regular, snapshot, application_available, saved_accounts,
            saved_accounts_error, current_credential_slot,
            current_environment, current_account_id, fonts.icons,
            account_popup);
        bool settings_changed = false;
        bool return_all_windows_to_workspace = false;
        DrawSettingsMenu(fonts.regular, application_settings, auto_connect,
                         settings_changed, return_all_windows_to_workspace);
        actions.settings_changed = settings_changed;
        actions.return_all_windows_to_workspace =
            return_all_windows_to_workspace;
        const CreationActions creation_actions =
            DrawCreationMenu(fonts.regular, workspace_state);
        actions.new_chart = creation_actions.new_chart;
        actions.new_watch_list = creation_actions.new_watch_list;
        actions.new_positions = creation_actions.new_positions;
        actions.new_orders = creation_actions.new_orders;
        actions.new_trade_hotkey = creation_actions.new_trade_hotkey;
        actions.new_events = creation_actions.new_events;
        actions.new_time_sales = creation_actions.new_time_sales;
        actions.open_watch_list_id = creation_actions.open_watch_list_id;

        metrics.interactive_left_width =
            static_cast<int>(ImGui::GetCursorPosX());

        if (snapshot.account) {
            const std::string summary = std::format(
                "BP ${:.0f} left  |  100% available",
                snapshot.account->buying_power.ToDisplayDouble());
            const float summary_width = ImGui::CalcTextSize(summary.c_str()).x;
            const float meter_x = std::max(
                static_cast<float>(metrics.interactive_left_width + 12),
                window_size.x - caption_controls_width - summary_width - 12.0f);
            ImGui::SetCursorPosX(meter_x);
            DrawBuyingPowerMeter(*snapshot.account);
        }

        ImGui::SetCursorPosX(window_size.x - caption_controls_width);
        const ImVec2 button_size{
            static_cast<float>(metrics.control_width), row_height};
        if (DrawChromeButton(
                "##minimize", ChromeButtonSymbol::Minimize, button_size))
            static_cast<void>(SDL_MinimizeWindow(window));
        ImGui::SameLine(0.0f, 0.0f);
        const bool maximized =
            (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
        if (DrawChromeButton(
                "##maximize",
                maximized ? ChromeButtonSymbol::Restore
                          : ChromeButtonSymbol::Maximize,
                button_size)) {
            if (maximized)
                static_cast<void>(SDL_RestoreWindow(window));
            else
                static_cast<void>(SDL_MaximizeWindow(window));
        }
        ImGui::SameLine(0.0f, 0.0f);
        if (DrawChromeButton(
                "##close", ChromeButtonSymbol::Close, button_size))
            done = true;

        ImGui::PushFont(fonts.title, 20.0f);
        const std::string time_title(market_time_text);
        const bool live_account =
            snapshot.authenticated &&
            snapshot.environment == core::AccountEnvironment::Live;
        std::string environment_title =
            snapshot.authenticated ? (live_account ? "LIVE" : "PAPER") : "";
        if (!environment_title.empty() && !account_alias.empty()) {
            environment_title += " · ";
            environment_title += account_alias;
        }
        const ImVec2 time_size = ImGui::CalcTextSize(time_title.c_str());
        const ImVec2 environment_size =
            ImGui::CalcTextSize(environment_title.c_str());
        const float title_gap = environment_title.empty() ? 0.0f : 12.0f;
        const float title_width =
            time_size.x + title_gap + environment_size.x;
        ImGui::SetCursorPosX((window_size.x - title_width) * 0.5f);
        ImGui::TextUnformatted(time_title.c_str());
        if (!environment_title.empty()) {
            ImGui::SameLine(0.0f, title_gap);
            ImGui::TextColored(
                live_account
                    ? ImVec4{0.30f, 0.85f, 0.40f, 1.0f}
                    : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                "%s", environment_title.c_str());
        }
        ImGui::PopFont();

        ImGui::EndMenuBar();

    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    return actions;
}

}  // namespace tradebox::gui
