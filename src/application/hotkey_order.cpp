#include "tradebox/application/hotkey_order.h"

#include "tradebox/core/market_data.h"
#include "tradebox/core/types.h"

#include <expected>

namespace tradebox::application {
namespace {
const core::Decimal kHundred = *core::Decimal::Parse("100");

std::expected<core::Decimal, std::string> Percentage(const core::Decimal& value,
                                                      const core::Decimal& percent) {
    const auto ratio = percent.Divide(kHundred, 9);
    if (!ratio) return std::unexpected("Invalid percentage");
    return value * *ratio;
}

core::Decimal Floor(const core::Decimal& value) {
    const std::string text = value.ToString();
    const auto point = text.find('.');
    return *core::Decimal::Parse(text.substr(0, point));
}
}  // namespace

std::expected<HotkeyBracketPreview, std::string> BuildHotkeyBracketPreview(
    const core::CoreSnapshot& account_snapshot,
    const core::MarketDataSnapshot& market,
    const HotkeyBracketIntent& intent,
    const core::PriceIncrementSchedule& price_increments) {
    if (intent.symbol.empty()) return std::unexpected("Select a ticker first");
    if (!account_snapshot.account) return std::unexpected("Account data is unavailable");
    if (!market.latest_quote) return std::unexpected("A current bid/ask quote is required");
    if (intent.risk_percent <= core::Decimal::Zero() ||
        intent.risk_percent > kHundred ||
        intent.target_percent <= core::Decimal::Zero() ||
        intent.stop_percent <= core::Decimal::Zero() ||
        intent.maximum_buying_power_percent <= core::Decimal::Zero() ||
        intent.maximum_buying_power_percent > kHundred)
        return std::unexpected("Risk, target, stop, and buying-power percentages must be between zero and 100");
    const bool is_long = intent.side == HotkeyOrderSide::Long;
    if (!is_long && !account_snapshot.account->shorting_enabled)
        return std::unexpected("Short selling is not enabled for this account");
    const core::Decimal reference = is_long ? market.latest_quote->ask_price : market.latest_quote->bid_price;
    if (reference <= core::Decimal::Zero()) return std::unexpected("Quote has no usable bid/ask price");
    const auto stop_distance = Percentage(reference, intent.stop_percent);
    const auto target_distance = Percentage(reference, intent.target_percent);
    const auto risk_budget = Percentage(account_snapshot.account->equity, intent.risk_percent);
    if (!stop_distance || !target_distance || !risk_budget) return std::unexpected("Unable to calculate bracket");
    const auto risk_quantity = risk_budget->Divide(*stop_distance, 9);
    const auto permitted_buying_power = Percentage(account_snapshot.account->buying_power, intent.maximum_buying_power_percent);
    if (!permitted_buying_power)
        return std::unexpected("Unable to calculate buying-power limit");
    const auto buying_power_quantity = permitted_buying_power->Divide(reference, 9);
    if (!risk_quantity || !buying_power_quantity)
        return std::unexpected("Unable to calculate quantity");
    const core::Decimal limiting_quantity =
        *risk_quantity < *buying_power_quantity ? *risk_quantity : *buying_power_quantity;
    const core::Decimal quantity = Floor(limiting_quantity);
    if (quantity <= core::Decimal::Zero()) return std::unexpected("Buying power or risk budget cannot purchase one share");
    const core::Decimal raw_target = is_long ? reference + *target_distance : reference - *target_distance;
    const core::Decimal raw_stop = is_long ? reference - *stop_distance : reference + *stop_distance;
    const auto target = core::QuantizePrice(
        raw_target, price_increments,
        is_long ? core::PriceRounding::Down : core::PriceRounding::Up);
    const auto stop = core::QuantizePrice(
        raw_stop, price_increments,
        is_long ? core::PriceRounding::Up : core::PriceRounding::Down);
    if (!target || !stop)
        return std::unexpected("Unable to apply the instrument price increment");
    HotkeyBracketPreview preview;
    preview.order.symbol = intent.symbol;
    preview.order.qty = quantity;
    preview.order.side = is_long ? core::OrderSide::Buy : core::OrderSide::Sell;
    preview.order.type = core::OrderType::Market;
    preview.order.time_in_force = intent.gtc ? core::TimeInForce::Gtc : core::TimeInForce::Day;
    preview.order.order_class = core::OrderClass::Bracket;
    preview.order.take_profit = core::TakeProfit{*target};
    preview.order.stop_loss = core::StopLoss{*stop};
    preview.reference_price = reference;
    preview.risk_budget = *risk_budget;
    preview.estimated_entry_notional = reference * quantity;
    const core::Decimal actual_stop_distance =
        is_long ? reference - *stop : *stop - reference;
    preview.estimated_loss = actual_stop_distance * quantity;
    preview.buying_power_limited = *buying_power_quantity < *risk_quantity;
    return preview;
}
}  // namespace tradebox::application
