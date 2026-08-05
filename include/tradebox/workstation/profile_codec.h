#pragma once

#include "tradebox/workstation/state.h"

#include <expected>
#include <string>
#include <string_view>

namespace tradebox::workstation {

[[nodiscard]] std::expected<WorkstationState, std::string> DecodeProfile(
    std::string_view source);
[[nodiscard]] std::string EncodeProfile(const WorkstationState& state);

}  // namespace tradebox::workstation

