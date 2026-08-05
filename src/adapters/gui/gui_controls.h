#pragma once

#include "imgui.h"

#include <array>
#include <cstddef>

namespace tradebox::gui {

enum class ChromeButtonSymbol {
    Minimize,
    Maximize,
    Restore,
    Close,
};

[[nodiscard]] std::array<char, 4> Utf8BmpGlyph(unsigned int codepoint);

[[nodiscard]] bool DrawChromeButton(
    const char* id, ChromeButtonSymbol symbol, ImVec2 size);
[[nodiscard]] bool DrawTitleBarToolButton(
    const char* id, unsigned int codepoint, ImFont* icon_font, ImVec2 size,
    ImU32 glyph_color);
[[nodiscard]] bool DrawVisibilityButton(
    const char* id, bool visible, ImFont* icon_font);

[[nodiscard]] bool DrawLabeledTextInput(
    const char* label, const char* id, const char* hint, char* buffer,
    std::size_t buffer_size, ImGuiInputTextFlags flags = 0);
[[nodiscard]] bool DrawLabeledSecretInput(
    const char* label, const char* id, const char* hint, char* buffer,
    std::size_t buffer_size, bool& visible, ImFont* icon_font);

}  // namespace tradebox::gui
