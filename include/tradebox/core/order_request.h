#pragma once

#include "tradebox/core/types.h"

#include <optional>
#include <string>
#include <vector>

namespace tradebox::core {

enum class AssetClass { Equity, Crypto, Option, Ipo };
enum class OrderSide { Buy, Sell };
enum class OrderType { Market, Limit, Stop, StopLimit, TrailingStop };
enum class TimeInForce { Day, Gtc, Opg, Cls, Ioc, Fok };
enum class OrderClass { Simple, Bracket, Oco, Oto, Mleg };
enum class PositionIntent {
    BuyToOpen,
    BuyToClose,
    SellToOpen,
    SellToClose,
};

struct TakeProfit {
    Decimal limit_price;
};

struct StopLoss {
    Decimal stop_price;
    std::optional<Decimal> limit_price;
};

struct OrderLeg {
    std::string symbol;
    Decimal ratio_qty;
    OrderSide side = OrderSide::Buy;
    PositionIntent position_intent = PositionIntent::BuyToOpen;
};

struct NativeOrderRequest {
    AssetClass asset_class = AssetClass::Equity;
    std::string symbol;
    std::optional<Decimal> qty;
    std::optional<Decimal> notional;
    std::optional<OrderSide> side;
    OrderType type = OrderType::Market;
    TimeInForce time_in_force = TimeInForce::Day;
    OrderClass order_class = OrderClass::Simple;
    std::optional<PositionIntent> position_intent;
    std::optional<Decimal> limit_price;
    std::optional<Decimal> stop_price;
    std::optional<Decimal> trail_price;
    std::optional<Decimal> trail_percent;
    std::optional<TakeProfit> take_profit;
    std::optional<StopLoss> stop_loss;
    std::vector<OrderLeg> legs;
    bool extended_hours = false;
    std::string client_order_id;
};

struct ReplaceOrderRequest {
    std::optional<Decimal> qty;
    std::optional<Decimal> notional;
    std::optional<TimeInForce> time_in_force;
    std::optional<Decimal> limit_price;
    std::optional<Decimal> stop_price;
    std::optional<Decimal> trail;
    std::optional<std::string> client_order_id;
};

struct OrderValidationError {
    std::string field;
    std::string message;
};

[[nodiscard]] std::vector<OrderValidationError> ValidateOrder(
    const NativeOrderRequest& request);
[[nodiscard]] std::vector<OrderValidationError> ValidateReplacement(
    const OrderState& current, const ReplaceOrderRequest& replacement);

}  // namespace tradebox::core
