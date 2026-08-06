#include "application_chrome.h"

#include "gui_controls.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>

namespace tradebox::gui {
namespace {

ImVec4 AccountIconColor(const core::CoreSnapshot& snapshot) {
    if (snapshot.authenticated)
        return {0.25f, 0.78f, 0.42f, 1.0f};
    if (snapshot.safety_status == core::SafetyStatus::Error ||
        snapshot.safety_status == core::SafetyStatus::Stale)
        return {0.95f, 0.62f, 0.16f, 1.0f};
    return {0.60f, 0.62f, 0.66f, 1.0f};
}

void DrawSettingsPopup(
    ImFont* regular_font, const char* id, ImVec2 anchor,
    workstation::ApplicationSettings& settings, bool& auto_connect,
    bool& changed) {
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Appearing, {0.0f, 0.0f});
    ImGui::SetNextWindowSizeConstraints({250.0f, 0.0f}, {360.0f, 640.0f});
    if (!ImGui::BeginPopup(id)) return;

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
    ImGui::PopFont();
    ImGui::EndPopup();
}

struct CreationActions {
    bool new_chart = false;
    bool new_watch_list = false;
    bool new_debug = false;
    bool imgui_demo = false;
};

CreationActions DrawCreationPopup(ImFont* regular_font, const char* id,
                                  ImVec2 anchor) {
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Appearing, {0.0f, 0.0f});
    ImGui::SetNextWindowSizeConstraints({220.0f, 0.0f}, {320.0f, 320.0f});
    if (!ImGui::BeginPopup(id)) return {};

    ImGui::PushFont(regular_font, 18.0f);
    const bool new_chart = ImGui::MenuItem("Chart", "");
    const bool new_watch_list = ImGui::MenuItem("Watch List", "");
    const bool new_debug = ImGui::MenuItem("Debug", "");
    const bool imgui_demo = ImGui::MenuItem("ImGui Demo", "");
    if (new_chart || new_watch_list || new_debug || imgui_demo)
        ImGui::CloseCurrentPopup();
    ImGui::PopFont();
    ImGui::EndPopup();
    return {new_chart, new_watch_list, new_debug, imgui_demo};
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
    workstation::ApplicationSettings& application_settings, bool& done) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    metrics.title_bar_height = 35;
    metrics.control_width = 46;
    metrics.tool_width = 40;
    const float row_height = static_cast<float>(metrics.title_bar_height);
    viewport->WorkPos = {viewport->Pos.x, viewport->Pos.y + row_height};
    viewport->WorkSize = {
        viewport->Size.x, std::max(0.0f, viewport->Size.y - row_height)};
    const float caption_controls_width =
        static_cast<float>(metrics.control_width * 3);
    const float tool_controls_width =
        static_cast<float>(metrics.tool_width * 3);
    metrics.interactive_left_width = static_cast<int>(tool_controls_width);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
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
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(
            window_min, window_max, ImGui::GetColorU32(ImGuiCol_MenuBarBg));
        draw_list->AddLine(
            {window_min.x, window_max.y - 1.0f},
            {window_max.x, window_max.y - 1.0f},
            ImGui::GetColorU32(ImGuiCol_Border));

        ImGui::SetCursorPos({0.0f, 0.0f});
        const ImVec2 tool_size{
            static_cast<float>(metrics.tool_width), row_height};
        const bool account_clicked = DrawTitleBarToolButton(
            "##account", 0xf20b, fonts.icons, tool_size,
            ImGui::GetColorU32(AccountIconColor(snapshot)));
        const ImVec2 account_anchor = ImGui::GetItemRectMax();
        ImGui::SameLine(0.0f, 0.0f);
        const bool settings_clicked = DrawTitleBarToolButton(
            "##settings", 0xe8b8, fonts.icons, tool_size,
            ImGui::GetColorU32(ImGuiCol_Text));
        ImGui::SameLine(0.0f, 0.0f);
        const bool create_clicked = DrawTitleBarToolButton(
            "##create", 0xe145, fonts.icons, tool_size,
            ImGui::GetColorU32(ImGuiCol_Text));

        if (account_clicked) ImGui::OpenPopup("##account_menu");
        if (settings_clicked) ImGui::OpenPopup("##settings_menu");
        if (create_clicked) ImGui::OpenPopup("##create_menu");
        const ImVec2 left_menu_anchor{window_min.x, account_anchor.y};
        actions.account = DrawAccountPopup(
            fonts.regular, "##account_menu", left_menu_anchor, snapshot,
            application_available, saved_accounts, saved_accounts_error,
            current_credential_slot, current_environment, current_account_id,
            fonts.icons,
            account_popup);
        DrawSettingsPopup(fonts.regular, "##settings_menu", left_menu_anchor,
                          application_settings,
                          auto_connect,
                          actions.settings_changed);
        const CreationActions creation_actions = DrawCreationPopup(
            fonts.regular, "##create_menu",
            {window_min.x + tool_controls_width -
                 static_cast<float>(metrics.tool_width),
             account_anchor.y});
        actions.new_chart = creation_actions.new_chart;
        actions.new_watch_list = creation_actions.new_watch_list;
        actions.new_debug = creation_actions.new_debug;
        actions.imgui_demo = creation_actions.imgui_demo;

        ImGui::SetCursorPos({window_size.x - caption_controls_width, 0.0f});
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
        const float title_x =
            window_min.x + (window_size.x - title_width) * 0.5f;
        draw_list->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(),
            {title_x, window_min.y + (row_height - time_size.y) * 0.5f},
            ImGui::GetColorU32(ImGuiCol_Text), time_title.c_str());
        if (!environment_title.empty()) {
            draw_list->AddText(
                ImGui::GetFont(), ImGui::GetFontSize(),
                {title_x + time_size.x + title_gap,
                 window_min.y + (row_height - environment_size.y) * 0.5f},
                live_account ? ImGui::GetColorU32(ImGuiCol_PlotLinesHovered)
                             : ImGui::GetColorU32(ImGuiCol_TextDisabled),
                environment_title.c_str());
        }
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    return actions;
}

}  // namespace tradebox::gui
