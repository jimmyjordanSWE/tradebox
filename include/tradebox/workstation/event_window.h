#pragma once
#include "tradebox/workstation/state.h"
#include <expected>
#include <string>
namespace tradebox::workstation {
inline constexpr std::string_view kEventWindowId = "events.window";
[[nodiscard]] std::expected<void, std::string> CreateEventWindow(WorkspaceState& state);
}
