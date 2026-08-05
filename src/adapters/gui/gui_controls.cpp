#include "gui_controls.h"

#include <algorithm>
#include <cmath>

namespace tradebox::gui {
namespace {

constexpr float kToolIconSize = 24.0f;

void DrawChromeButtonSymbol(
    ImDrawList& draw_list, const ImVec2& button_min, const ImVec2& button_size,
    ChromeButtonSymbol symbol, ImU32 color) {
    constexpr float kSymbolSize = 10.0f;
    constexpr float kStrokeWidth = 1.0f;
    constexpr float kCornerRadius = 1.5f;
    constexpr float kSeparation = 2.0f;
    const float left = std::floor(
        button_min.x + (button_size.x - kSymbolSize) * 0.5f) + 0.5f;
    const float top = std::floor(
        button_min.y + (button_size.y - kSymbolSize) * 0.5f) + 0.5f;
    const float right = left + kSymbolSize - 1.0f;
    const float bottom = top + kSymbolSize - 1.0f;

    switch (symbol) {
        case ChromeButtonSymbol::Minimize: {
            const float middle = std::floor((top + bottom) * 0.5f) + 0.5f;
            draw_list.AddLine({left, middle}, {right, middle}, color,
                              kStrokeWidth);
            break;
        }
        case ChromeButtonSymbol::Maximize:
            draw_list.AddRect({left, top}, {right, bottom}, color,
                              kCornerRadius, ImDrawFlags_RoundCornersAll,
                              kStrokeWidth);
            break;
        case ChromeButtonSymbol::Restore:
            draw_list.AddLine(
                {left + kSeparation, top}, {right, top}, color, kStrokeWidth);
            draw_list.AddLine(
                {right, top}, {right, bottom - kSeparation}, color,
                kStrokeWidth);
            draw_list.AddRect(
                {left, top + kSeparation},
                {right - kSeparation, bottom}, color, kCornerRadius,
                ImDrawFlags_RoundCornersAll, kStrokeWidth);
            break;
        case ChromeButtonSymbol::Close:
            draw_list.AddLine({left, top}, {right, bottom}, color,
                              kStrokeWidth);
            draw_list.AddLine({right, top}, {left, bottom}, color,
                              kStrokeWidth);
            break;
    }
}

}  // namespace

std::array<char, 4> Utf8BmpGlyph(unsigned int codepoint) {
    return {
        static_cast<char>(0xe0U | (codepoint >> 12U)),
        static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)),
        static_cast<char>(0x80U | (codepoint & 0x3fU)),
        '\0'};
}

bool DrawChromeButton(
    const char* id, ChromeButtonSymbol symbol, ImVec2 size) {
    const bool clicked = ImGui::InvisibleButton(id, size);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool close = symbol == ChromeButtonSymbol::Close;

    ImDrawList& draw_list = *ImGui::GetWindowDrawList();
    if (hovered || active) {
        ImU32 background = 0;
        if (close) {
            background = active ? IM_COL32(232, 17, 35, 0x98)
                                : IM_COL32(232, 17, 35, 0xff);
        } else {
            const ImVec4 foreground = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            background = ImGui::ColorConvertFloat4ToU32(
                {foreground.x, foreground.y, foreground.z,
                 active ? 0x33 / 255.0f : 0x1a / 255.0f});
        }
        draw_list.AddRectFilled(minimum, maximum, background);
    }

    const ImU32 symbol_color = close && (hovered || active)
                                   ? IM_COL32(255, 255, 255, 255)
                                   : ImGui::GetColorU32(ImGuiCol_Text);
    DrawChromeButtonSymbol(
        draw_list, minimum, size, symbol, symbol_color);
    return clicked;
}

bool DrawTitleBarToolButton(
    const char* id, unsigned int codepoint, ImFont* icon_font, ImVec2 size,
    ImU32 glyph_color) {
    const bool clicked = ImGui::InvisibleButton(id, size);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImDrawList& draw_list = *ImGui::GetWindowDrawList();

    if (hovered || active) {
        const ImVec4 foreground = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        draw_list.AddRectFilled(
            minimum, maximum,
            ImGui::ColorConvertFloat4ToU32(
                {foreground.x, foreground.y, foreground.z,
                 active ? 0x33 / 255.0f : 0x1a / 255.0f}));
    }

    const std::array<char, 4> glyph = Utf8BmpGlyph(codepoint);
    ImGui::PushFont(icon_font, kToolIconSize);
    const ImVec2 glyph_size = ImGui::CalcTextSize(glyph.data());
    draw_list.AddText(
        ImGui::GetFont(), ImGui::GetFontSize(),
        {minimum.x + (size.x - glyph_size.x) * 0.5f,
         minimum.y + (size.y - glyph_size.y) * 0.5f},
        glyph_color, glyph.data());
    ImGui::PopFont();
    return clicked;
}

bool DrawVisibilityButton(const char* id, bool visible, ImFont* icon_font) {
    const ImVec2 size{34.0f, ImGui::GetFrameHeight()};
    const bool clicked = ImGui::InvisibleButton(id, size);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        const ImVec4 foreground = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::GetWindowDrawList()->AddRectFilled(
            minimum, maximum,
            ImGui::ColorConvertFloat4ToU32(
                {foreground.x, foreground.y, foreground.z, 0.15f}));
    }
    const std::array<char, 4> glyph =
        Utf8BmpGlyph(visible ? 0xe8f4U : 0xe8f5U);
    ImGui::PushFont(icon_font, 20.0f);
    const ImVec2 glyph_size = ImGui::CalcTextSize(glyph.data());
    ImGui::GetWindowDrawList()->AddText(
        {minimum.x + (size.x - glyph_size.x) * 0.5f,
         minimum.y + (size.y - glyph_size.y) * 0.5f},
        ImGui::GetColorU32(ImGuiCol_Text), glyph.data());
    ImGui::PopFont();
    return clicked;
}

bool DrawLabeledTextInput(
    const char* label, const char* id, const char* hint, char* buffer,
    std::size_t buffer_size, ImGuiInputTextFlags flags) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);
    return ImGui::InputTextWithHint(
        id, hint, buffer, buffer_size, flags);
}

bool DrawLabeledSecretInput(
    const char* label, const char* id, const char* hint, char* buffer,
    std::size_t buffer_size, bool& visible, ImFont* icon_font) {
    ImGui::PushID(id);
    if (label[0] != '\0') ImGui::TextUnformatted(label);
    constexpr float eye_width = 34.0f;
    ImGui::SetNextItemWidth(
        std::max(120.0f, ImGui::GetContentRegionAvail().x -
                              eye_width - 6.0f));
    const bool edited = ImGui::InputTextWithHint(
        id, hint, buffer, buffer_size,
        visible ? 0 : ImGuiInputTextFlags_Password);
    ImGui::SameLine(0.0f, 6.0f);
    if (DrawVisibilityButton("##visibility", visible, icon_font))
        visible = !visible;
    ImGui::PopID();
    return edited;
}

}  // namespace tradebox::gui
