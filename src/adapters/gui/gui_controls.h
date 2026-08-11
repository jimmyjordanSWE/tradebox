#pragma once

#include "tradebox/workstation/state.h"

#include "imgui.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tradebox::gui {

enum class ChromeButtonSymbol {
    Minimize,
    Maximize,
    Restore,
    Close,
};

struct TableColumnChoice {
    std::string_view id;
    std::string_view label;
    bool required = false;
};

struct TableColumnActions {
    std::optional<std::string> add;
};

struct TableColumnLayout {
    std::string id;
    float width = 0.0f;
    bool visible = true;
};

[[nodiscard]] std::vector<workstation::ColumnState*>
OrderedTableColumns(workstation::PersistentTableState& table);
[[nodiscard]] std::vector<workstation::ColumnState*>
OrderedVisibleTableColumns(workstation::PersistentTableState& table);
[[nodiscard]] ImGuiTableFlags PersistentTableInteractionFlags();
[[nodiscard]] std::string TableColumnIdFromLabel(const char* label);
[[nodiscard]] bool PersistTableColumnOrder(
    workstation::PersistentTableState& table,
    std::span<const std::string> display_ids);
[[nodiscard]] bool PersistTableColumnLayout(
    workstation::PersistentTableState& table,
    std::span<const TableColumnLayout> display_columns);
[[nodiscard]] bool PersistCurrentTableLayout(
    workstation::PersistentTableState& table);
[[nodiscard]] std::vector<std::string> CurrentVisibleTableColumnIds();
[[nodiscard]] bool PersistTableSortSpecs(
    workstation::PersistentTableState& table,
    const ImGuiTableSortSpecs* sort_specs);
void SetupPersistentTableColumns(
    std::span<workstation::ColumnState* const> columns,
    std::span<const TableColumnChoice> choices, float fallback_width);
[[nodiscard]] TableColumnActions DrawTableColumnControls(
    const workstation::PersistentTableState& table,
    std::span<const TableColumnChoice> choices, std::string_view id_scope);

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
