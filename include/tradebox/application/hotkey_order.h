#pragma once

#include "tradebox/core/decimal.h"
#include "tradebox/core/order_request.h"
#include "tradebox/core/market_data.h"
#include "tradebox/core/price_increment.h"
#include "tradebox/core/types.h"

#include <expected>

#include <string>

namespace tradebox::application {

enum class HotkeyOrderSide { Long, Short };

// A user intent, not an order. TradingApplication is responsible for deriving
// the broker command from current account and market projections.
struct HotkeyBracketIntent {
    std::string symbol;
    HotkeyOrderSide side = HotkeyOrderSide::Long;
    core::Decimal risk_percent;
    core::Decimal target_percent;
    core::Decimal stop_percent;
    core::Decimal maximum_buying_power_percent;
    bool gtc = true;
};

struct HotkeyBracketPreview {
    core::NativeOrderRequest order;
    core::Decimal reference_price;
    core::Decimal risk_budget;
    core::Decimal estimated_entry_notional;
    core::Decimal estimated_loss;
    bool buying_power_limited = false;
};

[[nodiscard]] std::expected<HotkeyBracketPreview, std::string>
BuildHotkeyBracketPreview(const core::CoreSnapshot& account_snapshot,
                          const core::MarketDataSnapshot& market,
                          const HotkeyBracketIntent& intent,
                          const core::PriceIncrementSchedule& price_increments);

}  // namespace tradebox::application
