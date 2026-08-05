#include "gui_support.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>

namespace tradebox::gui {

void ConfigureImGuiStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 5.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;

    const ImVec4 neutral_button{0.24f, 0.25f, 0.27f, 1.0f};
    const ImVec4 neutral_button_hovered{0.32f, 0.33f, 0.35f, 1.0f};
    const ImVec4 neutral_button_active{0.38f, 0.39f, 0.41f, 1.0f};
    style.Colors[ImGuiCol_Button] = neutral_button;
    style.Colors[ImGuiCol_ButtonHovered] = neutral_button_hovered;
    style.Colors[ImGuiCol_ButtonActive] = neutral_button_active;
    style.Colors[ImGuiCol_Header] = neutral_button;
    style.Colors[ImGuiCol_HeaderHovered] = neutral_button_hovered;
    style.Colors[ImGuiCol_HeaderActive] = neutral_button_active;
    style.Colors[ImGuiCol_Tab] = neutral_button;
    style.Colors[ImGuiCol_TabHovered] = neutral_button_hovered;
    style.Colors[ImGuiCol_TabSelected] = neutral_button_active;
    style.Colors[ImGuiCol_CheckMark] = {0.78f, 0.79f, 0.81f, 1.0f};
    style.Colors[ImGuiCol_SliderGrab] = {0.60f, 0.61f, 0.63f, 1.0f};
    style.Colors[ImGuiCol_SliderGrabActive] = neutral_button_active;
}

void DrawImGuiDemo(bool& open, float title_bar_height) {
    if (!open) return;

    ImGui::ShowDemoWindow(&open);
    ImGuiWindow* demo_window = ImGui::FindWindowByName("Dear ImGui Demo");
    if (demo_window == nullptr) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float minimum_y = viewport->Pos.y + title_bar_height;
    if (demo_window->Pos.y < minimum_y)
        ImGui::SetWindowPos(
            demo_window->Name, {demo_window->Pos.x, minimum_y});
}

}  // namespace tradebox::gui
