#pragma once

#include "tradebox/core/order_request.h"

#include <expected>
#include <string>

namespace tradebox::broker::alpaca {

[[nodiscard]] std::expected<std::string, std::string> SerializeOrder(
    const core::NativeOrderRequest& request);
[[nodiscard]] std::expected<std::string, std::string>
SerializeReplacement(const core::ReplaceOrderRequest& request);

}  // namespace tradebox::broker::alpaca
