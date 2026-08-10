#include "tradebox/core/price_increment.h"

#include <string>

namespace tradebox::core {
namespace {

std::expected<Decimal, std::string> Truncate(const Decimal& value) {
    const std::string text = value.ToString();
    const std::size_t point = text.find('.');
    const auto parsed = Decimal::Parse(text.substr(0, point));
    if (!parsed) return std::unexpected("Unable to truncate decimal");
    return *parsed;
}

std::expected<Decimal, std::string> IncrementFor(
    const Decimal& price, const PriceIncrementSchedule& schedule) {
    if (price <= Decimal::Zero())
        return std::unexpected("Price must be greater than zero");
    if (schedule.fallback_increment <= Decimal::Zero())
        return std::unexpected("Price-increment schedule has no fallback increment");
    for (const PriceIncrementBand& band : schedule.bands) {
        if (band.upper_bound_exclusive <= Decimal::Zero() ||
            band.increment <= Decimal::Zero())
            return std::unexpected("Price-increment schedule contains an invalid band");
        if (price < band.upper_bound_exclusive) return band.increment;
    }
    return schedule.fallback_increment;
}

}  // namespace

std::expected<Decimal, std::string> QuantizePrice(
    const Decimal& price, const PriceIncrementSchedule& schedule,
    PriceRounding rounding) {
    const auto increment = IncrementFor(price, schedule);
    if (!increment) return std::unexpected(increment.error());
    const auto quotient = price.Divide(*increment, 9);
    if (!quotient) return std::unexpected("Unable to calculate price increment");
    const auto truncated = Truncate(*quotient);
    if (!truncated) return std::unexpected(truncated.error());
    const Decimal one = *Decimal::Parse("1");
    const Decimal units = rounding == PriceRounding::Up && *truncated < *quotient
                              ? *truncated + one
                              : *truncated;
    return units * *increment;
}

std::expected<bool, std::string> IsPriceIncrementAligned(
    const Decimal& price, const PriceIncrementSchedule& schedule) {
    const auto rounded = QuantizePrice(price, schedule, PriceRounding::Down);
    if (!rounded) return std::unexpected(rounded.error());
    return *rounded == price;
}

}  // namespace tradebox::core
