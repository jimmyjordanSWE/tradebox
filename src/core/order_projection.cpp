#include "tradebox/core/order_projection.h"

#include <algorithm>
#include <cctype>

namespace tradebox::core {

namespace {

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

Decimal WholeQuantityForBudget(const Decimal& budget,
                               const Decimal& price) {
    if (budget <= Decimal::Zero() || price <= Decimal::Zero())
        return Decimal::Zero();
    const auto ratio = budget.Divide(price, 9);
    if (!ratio) return Decimal::Zero();
    std::string whole = ratio->ToString();
    if (const std::size_t point = whole.find('.');
        point != std::string::npos)
        whole.resize(point);
    const auto parsed = Decimal::Parse(whole);
    return parsed ? *parsed : Decimal::Zero();
}

Decimal Budget(const Decimal& buying_power, const Decimal& percent) {
    const auto fraction = percent.Divide(*Decimal::Parse("100"), 9);
    return fraction ? buying_power * *fraction : Decimal::Zero();
}

Decimal PercentOf(const Decimal& value, const Decimal& percent) {
    const auto fraction = percent.Divide(*Decimal::Parse("100"), 9);
    return fraction ? value * *fraction : Decimal::Zero();
}

}  // namespace

OrderProjection ProjectOrder(const OrderState& order) {
    const std::string status = Lowercase(order.status);
    OrderLifecycleState lifecycle = OrderLifecycleState::Unknown;

    if (status == "filled") {
        lifecycle = OrderLifecycleState::Filled;
    } else if (status == "canceled" || status == "cancelled") {
        lifecycle = OrderLifecycleState::Canceled;
    } else if (status == "expired" || status == "rejected") {
        lifecycle = OrderLifecycleState::Rejected;
    } else if (status == "pending_new" || status == "pending_cancel" ||
               status == "pending_replace") {
        lifecycle = OrderLifecycleState::Pending;
    } else if (status == "new" || status == "accepted" ||
               status == "partially_filled" || status == "held" ||
               status == "stopped" || status == "suspended" ||
               status == "calculated") {
        lifecycle = OrderLifecycleState::Accepted;
    }

    // Cancellation and replacement are capabilities of an order's current
    // lifecycle, not UI guesses based on a particular table or window.
    const bool working = lifecycle == OrderLifecycleState::Pending ||
                         lifecycle == OrderLifecycleState::Accepted;
    return OrderProjection{
        .lifecycle = lifecycle,
        .capabilities = {.cancelable = working, .replaceable = working},
    };
}

QuickOrderProjection ProjectQuickOrder(
    const AccountState& account,
    const std::vector<PositionState>& positions,
    const MarketDataSnapshot& market,
    const Decimal& long_buying_power_percent,
    const Decimal& short_buying_power_percent) {
    const Decimal reference = market.latest_price
                                  ? market.latest_price->price
                                  : Decimal::Zero();
    QuickOrderProjection result{
        .reference_price = reference,
        .long_budget = Budget(account.buying_power,
                              long_buying_power_percent),
        .short_budget = Budget(account.buying_power,
                               short_buying_power_percent),
    };
    result.long_quantity = WholeQuantityForBudget(
        result.long_budget, reference);
    result.short_quantity = WholeQuantityForBudget(
        result.short_budget, reference);
    for (const PositionState& position : positions) {
        if (position.symbol == market.symbol &&
            position.qty_available > Decimal::Zero()) {
            if (position.side == "long")
                result.long_exit_quantity = position.qty_available;
            else if (position.side == "short")
                result.short_exit_quantity = position.qty_available;
        }
    }
    return result;
}

BracketProjection ProjectBracket(
    const Decimal& reference_price,
    const Decimal& target_percent,
    const Decimal& stop_percent,
    const Decimal& quantity,
    bool short_entry) {
    const Decimal target_delta = PercentOf(reference_price, target_percent);
    const Decimal stop_delta = PercentOf(reference_price, stop_percent);
    const Decimal target = short_entry
                               ? reference_price - target_delta
                               : reference_price + target_delta;
    const Decimal stop = short_entry
                             ? reference_price + stop_delta
                             : reference_price - stop_delta;
    const Decimal reward_distance = target >= reference_price
                                        ? target - reference_price
                                        : reference_price - target;
    const Decimal risk_distance = stop >= reference_price
                                      ? stop - reference_price
                                      : reference_price - stop;
    const Decimal reward = reward_distance * quantity;
    const Decimal risk = risk_distance * quantity;
    Decimal risk_reward = Decimal::Zero();
    if (!risk.IsZero()) {
        const auto ratio = reward.Divide(risk, 4);
        if (ratio) risk_reward = *ratio;
    }
    return {
        .target_price = target,
        .stop_price = stop,
        .reward = reward,
        .risk = risk,
        .risk_reward = risk_reward,
    };
}

}  // namespace tradebox::core
