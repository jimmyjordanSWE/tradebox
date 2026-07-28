#include "tradebox/core/order_request.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

namespace {

using namespace tradebox::core;

Decimal D(std::string_view value) {
    return *Decimal::Parse(value);
}

bool HasError(const std::vector<OrderValidationError>& errors,
              std::string_view field) {
    return std::ranges::any_of(
        errors, [field](const OrderValidationError& error) {
            return error.field == field;
        });
}

TEST(OrderRequest, AcceptsFractionalDayEquityLimitOrder) {
    NativeOrderRequest request{
        .asset_class = AssetClass::Equity,
        .symbol = "AAPL",
        .qty = D("0.123456789"),
        .side = OrderSide::Buy,
        .type = OrderType::Limit,
        .time_in_force = TimeInForce::Day,
        .limit_price = D("201.25"),
    };

    EXPECT_TRUE(ValidateOrder(request).empty());
}

TEST(OrderRequest, RejectsFractionalEquityWithGtc) {
    NativeOrderRequest request{
        .asset_class = AssetClass::Equity,
        .symbol = "AAPL",
        .qty = D("0.5"),
        .side = OrderSide::Buy,
        .type = OrderType::Market,
        .time_in_force = TimeInForce::Gtc,
    };

    EXPECT_TRUE(HasError(ValidateOrder(request), "time_in_force"));
}

TEST(OrderRequest, AcceptsFourLegOptionsStrategy) {
    NativeOrderRequest request{
        .asset_class = AssetClass::Option,
        .qty = D("1"),
        .type = OrderType::Limit,
        .time_in_force = TimeInForce::Day,
        .order_class = OrderClass::Mleg,
        .limit_price = D("-2.05"),
        .legs = {
            {"AAPL270115C00200000", D("1"), OrderSide::Buy,
             PositionIntent::BuyToOpen},
            {"AAPL270115C00205000", D("1"), OrderSide::Sell,
             PositionIntent::SellToOpen},
            {"AAPL270115P00190000", D("1"), OrderSide::Sell,
             PositionIntent::SellToOpen},
            {"AAPL270115P00185000", D("1"), OrderSide::Buy,
             PositionIntent::BuyToOpen},
        },
    };

    EXPECT_TRUE(ValidateOrder(request).empty());
}

TEST(OrderRequest, ValidatesOcoShape) {
    NativeOrderRequest request{
        .asset_class = AssetClass::Equity,
        .symbol = "AAPL",
        .qty = D("10"),
        .side = OrderSide::Sell,
        .type = OrderType::Limit,
        .time_in_force = TimeInForce::Gtc,
        .order_class = OrderClass::Oco,
        .take_profit = TakeProfit{D("220")},
        .stop_loss = StopLoss{D("190"), D("189.50")},
    };

    EXPECT_TRUE(ValidateOrder(request).empty());
}

TEST(OrderReplacement, RejectsNonIpoNotionalOrderChanges) {
    OrderState current;
    current.id = "order-1";
    current.asset_class = "us_equity";
    current.status = "new";
    current.type = "market";
    current.order_class = "simple";
    current.notional = D("100");

    EXPECT_TRUE(HasError(
        ValidateReplacement(
            current, ReplaceOrderRequest{.client_order_id = "replacement"}),
        "notional"));
}

TEST(OrderReplacement, RejectsFractionalQuantityChange) {
    OrderState current;
    current.id = "order-1";
    current.asset_class = "us_equity";
    current.status = "new";
    current.type = "limit";
    current.order_class = "simple";
    current.qty = D("0.5");

    EXPECT_TRUE(HasError(
        ValidateReplacement(current,
                            ReplaceOrderRequest{.qty = D("0.75")}),
        "qty"));
}

TEST(OrderReplacement, AcceptsTrailingStopTrailChange) {
    OrderState current;
    current.id = "order-1";
    current.asset_class = "us_equity";
    current.status = "new";
    current.type = "trailing_stop";
    current.order_class = "simple";
    current.qty = D("10");

    EXPECT_TRUE(
        ValidateReplacement(current,
                            ReplaceOrderRequest{.trail = D("1.25")})
            .empty());
}

TEST(OrderReplacement, RejectsOtoReplacement) {
    OrderState current;
    current.id = "order-1";
    current.asset_class = "us_equity";
    current.status = "new";
    current.type = "limit";
    current.order_class = "oto";
    current.qty = D("10");

    EXPECT_TRUE(HasError(
        ValidateReplacement(
            current, ReplaceOrderRequest{.limit_price = D("200")}),
        "order_class"));
}

}  // namespace
