#pragma once

#include "tradebox/workstation/state.h"

#include <expected>
#include <string>
#include <string_view>
#include <variant>

namespace tradebox::workstation {

struct BindWindowInstrumentLink {
    std::string window_id;
    // Empty unlinks the window and restores its local instrument context.
    std::string group_id;
};

struct RenameInstrumentLinkGroup {
    std::string group_id;
    std::string name;
};

struct SelectInstrumentLinkGroupInstrument {
    std::string group_id;
    InstrumentSelectionState instrument;
};

struct ClearInstrumentLinkGroupInstrument {
    std::string group_id;
};

using InstrumentLinkCommand = std::variant<
    BindWindowInstrumentLink,
    RenameInstrumentLinkGroup,
    SelectInstrumentLinkGroupInstrument,
    ClearInstrumentLinkGroupInstrument>;

[[nodiscard]] std::array<InstrumentLinkGroupState,
                         kInstrumentLinkGroupCount>
DefaultInstrumentLinkGroups();
[[nodiscard]] std::string_view InstrumentLinkColorName(
    InstrumentLinkColor color);
[[nodiscard]] std::string InstrumentLinkGroupId(
    InstrumentLinkColor color);
[[nodiscard]] InstrumentLinkGroupState* FindInstrumentLinkGroup(
    WorkspaceState& state, std::string_view group_id);
[[nodiscard]] const InstrumentLinkGroupState* FindInstrumentLinkGroup(
    const WorkspaceState& state, std::string_view group_id);
[[nodiscard]] const InstrumentSelectionState* LinkedInstrumentForWindow(
    const WorkspaceState& state, std::string_view window_id);
[[nodiscard]] std::expected<bool, std::string> ApplyInstrumentLinkCommand(
    WorkspaceState& state, const InstrumentLinkCommand& command);

}  // namespace tradebox::workstation
