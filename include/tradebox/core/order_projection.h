#pragma once

#include "tradebox/core/types.h"
#include "tradebox/core/market_data.h"

#include <optional>
#include <vector>

namespace tradebox::core {

// The broker exposes a string status vocabulary, but the rest of the
// application should not have to interpret that vocabulary independently.
enum class OrderLifecycleState {
    Pending,
    Accepted,
    Rejected,
    Canceled,
    Filled,
    Unknown,
};

struct OrderCapabilities {
    bool cancelable = false;
    bool replaceable = false;
};

struct OrderProjection {
    OrderLifecycleState lifecycle = OrderLifecycleState::Unknown;
    OrderCapabilities capabilities;
};

[[nodiscard]] OrderProjection ProjectOrder(const OrderState& order);

struct QuickOrderProjection {
    Decimal reference_price;
    Decimal long_budget;
    Decimal short_budget;
    Decimal long_quantity;
    Decimal short_quantity;
    std::optional<Decimal> long_exit_quantity;
    std::optional<Decimal> short_exit_quantity;
};

struct BracketProjection {
    Decimal target_price;
    Decimal stop_price;
    Decimal reward;
    Decimal risk;
    Decimal risk_reward;
};

[[nodiscard]] QuickOrderProjection ProjectQuickOrder(
    const AccountState& account,
    const std::vector<PositionState>& positions,
    const MarketDataSnapshot& market,
    const Decimal& long_buying_power_percent,
    const Decimal& short_buying_power_percent);
[[nodiscard]] BracketProjection ProjectBracket(
    const Decimal& reference_price,
    const Decimal& target_percent,
    const Decimal& stop_percent,
    const Decimal& quantity,
    bool short_entry);

}  // namespace tradebox::core
