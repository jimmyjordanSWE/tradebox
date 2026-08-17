#include "buying_power_display.h"

#include <format>

namespace tradebox::gui {

BuyingPowerDisplay BuildBuyingPowerDisplay(
    const core::AccountState& account) {
    const std::string multiplier =
        account.multiplier.empty() ? "--" : account.multiplier + "x";
    return {
        .summary = std::format(
            "BP ${:.0f} available",
            account.buying_power.ToDisplayDouble()),
        .tooltip = std::format(
            "Broker-reported buying power: ${:.2f}\n"
            "Equity: ${:.2f}\n"
            "Reg T buying power: ${:.2f}\n"
            "Non-marginable buying power: ${:.2f}\n"
            "Account multiplier: {}\n\n"
            "Buying power can change with positions, margin requirements, "
            "and the trading session.",
            account.buying_power.ToDisplayDouble(),
            account.equity.ToDisplayDouble(),
            account.regt_buying_power.ToDisplayDouble(),
            account.non_marginable_buying_power.ToDisplayDouble(),
            multiplier),
    };
}

}  // namespace tradebox::gui
