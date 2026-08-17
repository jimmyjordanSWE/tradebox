#pragma once

#include "tradebox/core/types.h"

#include <string>

namespace tradebox::gui {

struct BuyingPowerDisplay {
    std::string summary;
    std::string tooltip;
};

// Formats Alpaca's broker-authoritative account fields without inventing a
// percentage whose denominator changes between margin/trading sessions.
[[nodiscard]] BuyingPowerDisplay BuildBuyingPowerDisplay(
    const core::AccountState& account);

}  // namespace tradebox::gui
