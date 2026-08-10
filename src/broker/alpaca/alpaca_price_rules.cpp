#include "tradebox/broker/alpaca_price_rules.h"

#include <string_view>

namespace tradebox::broker::alpaca {
namespace {

core::Decimal D(std::string_view value) {
    return *core::Decimal::Parse(value);
}

std::expected<void, std::string> ValidatePrice(
    const core::Decimal& price, std::string_view field) {
    const auto aligned = core::IsPriceIncrementAligned(
        price, UsEquityPriceIncrementSchedule());
    if (!aligned) return std::unexpected(std::string(field) + ": " + aligned.error());
    if (!*aligned)
        return std::unexpected(std::string(field) +
            ": must use $0.01 increments at or above $1.00 and $0.0001 below $1.00");
    return {};
}

}  // namespace

const core::PriceIncrementSchedule& UsEquityPriceIncrementSchedule() {
    static const core::PriceIncrementSchedule schedule{
        .bands = {{.upper_bound_exclusive = D("1"), .increment = D("0.0001")}},
        .fallback_increment = D("0.01")};
    return schedule;
}

std::expected<void, std::string> ValidateOrderPriceIncrements(
    const core::NativeOrderRequest& request) {
    if (request.asset_class != core::AssetClass::Equity) return {};
    if (request.limit_price) {
        if (const auto result = ValidatePrice(*request.limit_price, "limit_price"); !result)
            return result;
    }
    if (request.stop_price) {
        if (const auto result = ValidatePrice(*request.stop_price, "stop_price"); !result)
            return result;
    }
    if (request.take_profit) {
        if (const auto result = ValidatePrice(request.take_profit->limit_price,
                                               "take_profit.limit_price"); !result)
            return result;
    }
    if (request.stop_loss) {
        if (const auto result = ValidatePrice(request.stop_loss->stop_price,
                                               "stop_loss.stop_price"); !result)
            return result;
        if (request.stop_loss->limit_price) {
            if (const auto result = ValidatePrice(*request.stop_loss->limit_price,
                                                   "stop_loss.limit_price"); !result)
                return result;
        }
    }
    return {};
}

}  // namespace tradebox::broker::alpaca
