#pragma once

#include "tradebox/workstation/state.h"

#include <string>

namespace tradebox::workstation {

bool ValidateAndNormalize(WorkstationState& state, std::string& error);

}  // namespace tradebox::workstation

