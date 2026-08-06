#include "tradebox/workstation/instrument_links.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <type_traits>

namespace tradebox::workstation {
namespace {

constexpr std::array<std::string_view, kInstrumentLinkGroupCount> kColorNames{
    "Red",      "Crimson", "Orange",   "Amber",  "Yellow", "Lime",
    "Green",    "Emerald", "Teal",     "Cyan",   "Sky",    "Blue",
    "Indigo",   "Violet",  "Purple",   "Magenta", "Rose",  "Coral",
    "Peach",    "Gold",    "Olive",    "Mint",   "Aqua",   "Navy",
    "Lavender", "Plum",    "Maroon",   "Brown",  "Slate",  "Gray",
    "Black",    "White",
};

std::string Slug(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value)
        result.push_back(static_cast<char>(std::tolower(character)));
    return result;
}

}  // namespace

std::string_view InstrumentLinkColorName(InstrumentLinkColor color) {
    const auto index = static_cast<std::size_t>(color);
    return index < kColorNames.size() ? kColorNames[index]
                                      : std::string_view{"Unknown"};
}

std::string InstrumentLinkGroupId(InstrumentLinkColor color) {
    return "instrument-link." + Slug(InstrumentLinkColorName(color));
}

std::array<InstrumentLinkGroupState, kInstrumentLinkGroupCount>
DefaultInstrumentLinkGroups() {
    std::array<InstrumentLinkGroupState, kInstrumentLinkGroupCount> result;
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto color = static_cast<InstrumentLinkColor>(index);
        result[index] = {
            .id = InstrumentLinkGroupId(color),
            .name = std::string(InstrumentLinkColorName(color)),
            .color = color,
        };
    }
    return result;
}

InstrumentLinkGroupState* FindInstrumentLinkGroup(
    WorkspaceState& state, std::string_view group_id) {
    const auto found = std::ranges::find(
        state.instrument_link_groups, group_id,
        &InstrumentLinkGroupState::id);
    return found == state.instrument_link_groups.end() ? nullptr : &*found;
}

const InstrumentLinkGroupState* FindInstrumentLinkGroup(
    const WorkspaceState& state, std::string_view group_id) {
    const auto found = std::ranges::find(
        state.instrument_link_groups, group_id,
        &InstrumentLinkGroupState::id);
    return found == state.instrument_link_groups.end() ? nullptr : &*found;
}

const InstrumentSelectionState* LinkedInstrumentForWindow(
    const WorkspaceState& state, std::string_view window_id) {
    const auto window = state.windows.find(window_id);
    if (window == state.windows.end() ||
        window->second.instrument_link_group_id.empty())
        return nullptr;
    const auto* group = FindInstrumentLinkGroup(
        state, window->second.instrument_link_group_id);
    return group != nullptr && group->selected_instrument
               ? &*group->selected_instrument
               : nullptr;
}

std::expected<bool, std::string> ApplyInstrumentLinkCommand(
    WorkspaceState& state, const InstrumentLinkCommand& command) {
    return std::visit(
        [&state](const auto& typed) -> std::expected<bool, std::string> {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, BindWindowInstrumentLink>) {
                const auto window = state.windows.find(typed.window_id);
                if (window == state.windows.end())
                    return std::unexpected("Instrument link window does not exist");
                if (!typed.group_id.empty() &&
                    FindInstrumentLinkGroup(state, typed.group_id) == nullptr)
                    return std::unexpected("Instrument link group does not exist");
                if (window->second.instrument_link_group_id == typed.group_id)
                    return false;
                window->second.instrument_link_group_id = typed.group_id;
                return true;
            } else {
                InstrumentLinkGroupState* group = FindInstrumentLinkGroup(
                    state, typed.group_id);
                if (group == nullptr)
                    return std::unexpected("Instrument link group does not exist");
                if constexpr (std::is_same_v<T, RenameInstrumentLinkGroup>) {
                    if (typed.name.empty())
                        return std::unexpected("Instrument link group name is required");
                    if (group->name == typed.name) return false;
                    group->name = typed.name;
                    return true;
                } else if constexpr (
                    std::is_same_v<T, SelectInstrumentLinkGroupInstrument>) {
                    if (typed.instrument.instrument_id.empty() ||
                        typed.instrument.symbol.empty())
                        return std::unexpected(
                            "Linked instrument requires stable identity and symbol");
                    if (group->selected_instrument == typed.instrument)
                        return false;
                    group->selected_instrument = typed.instrument;
                    return true;
                } else {
                    if (!group->selected_instrument) return false;
                    group->selected_instrument.reset();
                    return true;
                }
            }
        },
        command);
}

}  // namespace tradebox::workstation
