#pragma once

#include "tradebox/core/decimal.h"

#include <expected>
#include <vector>

namespace tradebox::core {

enum class PriceRounding { Down, Up };

struct PriceIncrementBand {
    Decimal upper_bound_exclusive;
    Decimal increment;
};

struct PriceIncrementSchedule {
    std::vector<PriceIncrementBand> bands;
    Decimal fallback_increment;
};

[[nodiscard]] std::expected<Decimal, std::string> QuantizePrice(
    const Decimal& price, const PriceIncrementSchedule& schedule,
    PriceRounding rounding);
[[nodiscard]] std::expected<bool, std::string> IsPriceIncrementAligned(
    const Decimal& price, const PriceIncrementSchedule& schedule);

}  // namespace tradebox::core
