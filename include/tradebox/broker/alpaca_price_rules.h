#pragma once

#include "tradebox/core/order_request.h"
#include "tradebox/core/price_increment.h"

#include <expected>
#include <string>

namespace tradebox::broker::alpaca {

[[nodiscard]] const core::PriceIncrementSchedule&
UsEquityPriceIncrementSchedule();
[[nodiscard]] std::expected<void, std::string> ValidateOrderPriceIncrements(
    const core::NativeOrderRequest& request);

}  // namespace tradebox::broker::alpaca
