#include "gui_controls.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <ranges>

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

std::vector<workstation::ColumnState*> OrderedTableColumns(
    workstation::PersistentTableState& table) {
    std::vector<workstation::ColumnState*> columns;
    columns.reserve(table.columns.size());
    for (workstation::ColumnState& column : table.columns)
        columns.push_back(&column);
    std::ranges::stable_sort(columns, [](const auto* left, const auto* right) {
        if (left->order != right->order) return left->order < right->order;
        return left->id < right->id;
    });
    return columns;
}

std::vector<workstation::ColumnState*> OrderedVisibleTableColumns(
    workstation::PersistentTableState& table) {
    auto columns = OrderedTableColumns(table);
    std::erase_if(columns, [](const workstation::ColumnState* column) {
        return !column->visible;
    });
    return columns;
}

ImGuiTableFlags PersistentTableInteractionFlags() {
    return ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
           ImGuiTableFlags_Hideable |
           ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit |
           ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
           ImGuiTableFlags_NoSavedSettings |
           ImGuiTableFlags_ContextMenuInBody;
}

std::string TableColumnIdFromLabel(const char* label) {
    if (label == nullptr) return {};
    const std::string_view value(label);
    const std::size_t separator = value.find("###");
    return std::string(value.substr(
        separator == std::string_view::npos ? 0 : separator + 3));
}

bool PersistTableColumnOrder(
    workstation::PersistentTableState& table,
    std::span<const std::string> display_ids) {
    const std::size_t visible_count = static_cast<std::size_t>(
        std::ranges::count(table.columns, true,
                           &workstation::ColumnState::visible));
    if (display_ids.size() != visible_count) return false;

    std::vector<workstation::ColumnState> reordered;
    reordered.reserve(table.columns.size());
    for (const std::string& id : display_ids) {
        const auto found = std::ranges::find(
            table.columns, id, &workstation::ColumnState::id);
        if (id.empty() || found == table.columns.end() || !found->visible ||
            std::ranges::find(reordered, id,
                              &workstation::ColumnState::id) != reordered.end())
            return false;
        reordered.push_back(*found);
    }
    for (const workstation::ColumnState& column : table.columns) {
        if (!column.visible) reordered.push_back(column);
    }

    bool changed = false;
    for (std::size_t index = 0; index < reordered.size(); ++index) {
        changed = changed || reordered[index].order !=
                                 static_cast<int>(index) ||
                  reordered[index].id != table.columns[index].id;
        reordered[index].order = static_cast<int>(index);
    }
    if (changed) table.columns = std::move(reordered);
    return changed;
}

bool PersistTableColumnLayout(
    workstation::PersistentTableState& table,
    std::span<const TableColumnLayout> display_columns) {
    if (display_columns.size() != table.columns.size()) return false;

    std::vector<std::string> display_ids;
    display_ids.reserve(display_columns.size());
    for (const TableColumnLayout& layout : display_columns) {
        const auto found = std::ranges::find(
            table.columns, layout.id, &workstation::ColumnState::id);
        if (layout.id.empty() || found == table.columns.end() ||
            !std::isfinite(layout.width) ||
            layout.width <= 0.0f ||
            std::ranges::find(display_ids, layout.id) != display_ids.end())
            return false;
        display_ids.push_back(layout.id);
    }

    std::vector<workstation::ColumnState> reordered;
    reordered.reserve(table.columns.size());
    bool changed = false;
    for (std::size_t index = 0; index < display_columns.size(); ++index) {
        const TableColumnLayout& layout = display_columns[index];
        const auto found = std::ranges::find(
            table.columns, layout.id, &workstation::ColumnState::id);
        workstation::ColumnState column = *found;
        changed = changed || column.id != table.columns[index].id ||
                  column.order != static_cast<int>(index) ||
                  std::fabs(column.width - layout.width) > 0.5f ||
                  column.visible != layout.visible;
        column.order = static_cast<int>(index);
        column.width = layout.width;
        column.visible = layout.visible;
        reordered.push_back(std::move(column));
    }
    if (changed) table.columns = std::move(reordered);
    return changed;
}

bool PersistCurrentTableLayout(
    workstation::PersistentTableState& table) {
    const ImGuiTable* current = ImGui::GetCurrentTable();
    if (current == nullptr || current->ColumnsCount <= 0) return false;
    std::vector<TableColumnLayout> display_columns(
        static_cast<std::size_t>(current->ColumnsCount));
    for (int index = 0; index < current->ColumnsCount; ++index) {
        const ImGuiTableColumn& column = current->Columns[index];
        if (column.DisplayOrder < 0 ||
            column.DisplayOrder >= current->ColumnsCount)
            return false;
        display_columns[static_cast<std::size_t>(column.DisplayOrder)] = {
            .id = TableColumnIdFromLabel(
                ImGui::TableGetColumnName(index)),
            .width = column.WidthRequest > 0.0f
                         ? column.WidthRequest
                         : column.WidthGiven,
            .visible = column.IsUserEnabledNextFrame,
        };
    }
    return PersistTableColumnLayout(table, display_columns);
}

std::vector<std::string> CurrentVisibleTableColumnIds() {
    const ImGuiTable* current = ImGui::GetCurrentTable();
    if (current == nullptr || current->ColumnsCount <= 0) return {};
    std::vector<std::pair<int, std::string>> ordered;
    ordered.reserve(static_cast<std::size_t>(current->ColumnsCount));
    for (int index = 0; index < current->ColumnsCount; ++index) {
        const ImGuiTableColumn& column = current->Columns[index];
        if (!column.IsUserEnabled) continue;
        ordered.emplace_back(
            column.DisplayOrder,
            TableColumnIdFromLabel(ImGui::TableGetColumnName(index)));
    }
    std::ranges::sort(ordered, {}, &std::pair<int, std::string>::first);
    std::vector<std::string> ids;
    ids.reserve(ordered.size());
    for (auto& [order, id] : ordered) {
        static_cast<void>(order);
        ids.push_back(std::move(id));
    }
    return ids;
}

bool PersistTableSortSpecs(
    workstation::PersistentTableState& table,
    const ImGuiTableSortSpecs* sort_specs) {
    if (sort_specs == nullptr || sort_specs->SpecsCount == 0) return false;
    const ImGuiTableColumnSortSpecs& spec = sort_specs->Specs[0];
    const std::string id = TableColumnIdFromLabel(
        ImGui::TableGetColumnName(spec.ColumnIndex));
    if (id.empty()) return false;
    const std::string direction =
        spec.SortDirection == ImGuiSortDirection_Descending ? "descending"
                                                            : "ascending";
    bool changed = false;
    for (workstation::ColumnState& column : table.columns) {
        const std::string next = column.id == id ? direction : "";
        if (column.sort_direction != next) {
            column.sort_direction = next;
            changed = true;
        }
    }
    return changed;
}

void SetupPersistentTableColumns(
    std::span<workstation::ColumnState* const> columns,
    std::span<const TableColumnChoice> choices, float fallback_width) {
    for (const workstation::ColumnState* column : columns) {
        const auto choice = std::ranges::find(
            choices, column->id, &TableColumnChoice::id);
        const std::string_view text =
            choice == choices.end() ? std::string_view(column->id)
                                    : choice->label;
        const std::string label =
            std::string(text) + "###" + column->id;
        ImGuiTableColumnFlags flags = ImGuiTableColumnFlags_WidthFixed;
        if (!column->visible)
            flags |= ImGuiTableColumnFlags_DefaultHide;
        if (choice != choices.end() && choice->required)
            flags |= ImGuiTableColumnFlags_NoHide;
        if (column->sort_direction == "descending")
            flags |= ImGuiTableColumnFlags_DefaultSort |
                     ImGuiTableColumnFlags_PreferSortDescending;
        else if (column->sort_direction == "ascending")
            flags |= ImGuiTableColumnFlags_DefaultSort;
        ImGui::TableSetupColumn(
            label.c_str(), flags,
            column->width > 0.0f ? column->width : fallback_width);
    }
    ImGuiTable* current = ImGui::GetCurrentTable();
    if (current == nullptr || current->ColumnsCount !=
                                  static_cast<int>(columns.size()))
        return;
    for (int index = 0; index < current->ColumnsCount; ++index) {
        ImGuiTableColumn& runtime = current->Columns[index];
        const bool requested = columns[static_cast<std::size_t>(index)]->visible;
        if (runtime.IsUserEnabled == runtime.IsUserEnabledNextFrame &&
            runtime.IsUserEnabled != requested)
            ImGui::TableSetColumnEnabled(index, requested);
    }
}

TableColumnActions DrawTableColumnControls(
    const workstation::PersistentTableState& table,
    std::span<const TableColumnChoice> choices, std::string_view id_scope) {
    TableColumnActions actions;
    const std::string scope(id_scope);
    ImGui::PushID(scope.c_str());
    if (ImGui::Button("Add Column##add_column_button"))
        ImGui::OpenPopup("add_column");

    if (ImGui::BeginPopup("add_column")) {
        for (const TableColumnChoice& choice : choices) {
            const auto existing = std::ranges::find(
                table.columns, choice.id, &workstation::ColumnState::id);
            if (choice.id.empty() || choice.label.empty() || choice.required ||
                (existing != table.columns.end() && existing->visible))
                continue;
            const std::string label(choice.label);
            if (ImGui::MenuItem(label.c_str())) {
                actions.add = std::string(choice.id);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return actions;
}

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
