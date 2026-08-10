#include "application_chrome.h"

#include "gui_controls.h"

#include <SDL3/SDL.h>

#include <algorithm>
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
    std::optional<std::string> open_watch_list_id;
    bool new_debug = false;
    bool imgui_demo = false;
};

CreationActions DrawCreationMenu(
    ImFont* regular_font, const workstation::WorkspaceState& state) {
    CreationActions actions;
    if (!ImGui::BeginMenu("Add Window")) return actions;

    ImGui::PushFont(regular_font, 18.0f);
    if (ImGui::MenuItem("Chart", "")) actions.new_chart = true;
    if (ImGui::BeginMenu("Watchlist")) {
        for (const workstation::WatchListDocumentState& watch_list :
             state.watch_lists) {
            if (ImGui::MenuItem(watch_list.name.c_str()))
                actions.open_watch_list_id = watch_list.id;
        }
        if (!state.watch_lists.empty()) ImGui::Separator();
        if (ImGui::MenuItem("Add new...")) actions.new_watch_list = true;
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Positions", "")) actions.new_positions = true;
    if (ImGui::MenuItem("Orders", "")) actions.new_orders = true;
    if (ImGui::MenuItem("Debug", "")) actions.new_debug = true;
    if (ImGui::MenuItem("ImGui Demo", "")) actions.imgui_demo = true;
    ImGui::PopFont();
    ImGui::EndMenu();
    return actions;
}

void DrawSettingsMenu(ImFont* regular_font,
                      workstation::ApplicationSettings& settings,
                      bool& auto_connect, bool& changed) {
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
        const ImVec2 window_min = ImGui::GetWindowPos();
        const ImVec2 window_size = ImGui::GetWindowSize();
        const ImVec2 window_max{window_min.x + window_size.x,
                                window_min.y + window_size.y};

        ImGui::BeginMenuBar();

        actions.account = DrawAccountMenu(
            fonts.regular, snapshot, application_available, saved_accounts,
            saved_accounts_error, current_credential_slot,
            current_environment, current_account_id, fonts.icons,
            account_popup);
        bool settings_changed = false;
        DrawSettingsMenu(fonts.regular, application_settings, auto_connect,
                         settings_changed);
        actions.settings_changed = settings_changed;
        const CreationActions creation_actions =
            DrawCreationMenu(fonts.regular, workspace_state);
        actions.new_chart = creation_actions.new_chart;
        actions.new_watch_list = creation_actions.new_watch_list;
        actions.new_positions = creation_actions.new_positions;
        actions.new_orders = creation_actions.new_orders;
        actions.open_watch_list_id = creation_actions.open_watch_list_id;
        actions.new_debug = creation_actions.new_debug;
        actions.imgui_demo = creation_actions.imgui_demo;

        metrics.interactive_left_width =
            static_cast<int>(ImGui::GetCursorPosX());

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

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddLine(
            {window_min.x, window_max.y - 1.0f},
            {window_max.x, window_max.y - 1.0f},
            ImGui::GetColorU32(ImGuiCol_Border));
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    return actions;
}

}  // namespace tradebox::gui
