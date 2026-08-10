#include "tradebox/broker/alpaca_order_codec.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <optional>

namespace {

using namespace tradebox;

core::Decimal D(const char* value) {
    return *core::Decimal::Parse(value);
}

TEST(AlpacaOrderCodec, SerializesDecimalsAsExactJsonStrings) {
    core::NativeOrderRequest request{
        .asset_class = core::AssetClass::Equity,
        .symbol = "AAPL",
        .qty = D("0.123456789"),
        .side = core::OrderSide::Buy,
        .type = core::OrderType::Limit,
        .time_in_force = core::TimeInForce::Day,
        .limit_price = D("201.25"),
        .client_order_id = "tb-request-1",
    };

    const auto encoded = broker::alpaca::SerializeOrder(request);

    ASSERT_TRUE(encoded);
    const auto json = nlohmann::json::parse(*encoded);
    EXPECT_EQ(json.at("qty"), "0.123456789");
    EXPECT_EQ(json.at("limit_price"), "201.25");
    EXPECT_EQ(json.at("client_order_id"), "tb-request-1");
}

TEST(AlpacaOrderCodec, SerializesMultilegNetCreditWithoutParentSide) {
    core::NativeOrderRequest request{
        .asset_class = core::AssetClass::Option,
        .qty = D("1"),
        .type = core::OrderType::Limit,
        .time_in_force = core::TimeInForce::Day,
        .order_class = core::OrderClass::Mleg,
        .limit_price = D("-1.25"),
        .legs =
            {
                {"AAPL270115C00200000", D("1"), core::OrderSide::Buy,
                 core::PositionIntent::BuyToOpen},
                {"AAPL270115C00205000", D("1"),
                 core::OrderSide::Sell,
                 core::PositionIntent::SellToOpen},
            },
    };

    const auto encoded = broker::alpaca::SerializeOrder(request);

    ASSERT_TRUE(encoded);
    const auto json = nlohmann::json::parse(*encoded);
    EXPECT_FALSE(json.contains("symbol"));
    EXPECT_FALSE(json.contains("side"));
    EXPECT_EQ(json.at("limit_price"), "-1.25");
    EXPECT_EQ(json.at("legs").size(), 2U);
}

TEST(AlpacaOrderCodec, SerializesOcoExitAsNestedProfitAndStopLegs) {
    core::NativeOrderRequest request{
        .asset_class = core::AssetClass::Equity,
        .symbol = "AAPL",
        .qty = D("10"),
        .side = core::OrderSide::Sell,
        .type = core::OrderType::Limit,
        .time_in_force = core::TimeInForce::Gtc,
        .order_class = core::OrderClass::Oco,
        .take_profit = core::TakeProfit{D("210.00")},
        .stop_loss = core::StopLoss{D("195.00"), std::nullopt},
    };

    const auto encoded = broker::alpaca::SerializeOrder(request);

    ASSERT_TRUE(encoded);
    const auto json = nlohmann::json::parse(*encoded);
    EXPECT_EQ(json.at("order_class"), "oco");
    EXPECT_EQ(json.at("take_profit").at("limit_price"), "210");
    EXPECT_EQ(json.at("stop_loss").at("stop_price"), "195");
    EXPECT_FALSE(json.contains("limit_price"));
}

TEST(AlpacaOrderCodec, RejectsEquityPricesThatDoNotMatchAlpacaIncrements) {
    core::NativeOrderRequest request{
        .asset_class = core::AssetClass::Equity,
        .symbol = "AAPL",
        .qty = D("10"),
        .side = core::OrderSide::Buy,
        .type = core::OrderType::Market,
        .time_in_force = core::TimeInForce::Gtc,
        .order_class = core::OrderClass::Bracket,
        .take_profit = core::TakeProfit{D("309.42576")},
        .stop_loss = core::StopLoss{D("308.81"), std::nullopt},
    };

    const auto encoded = broker::alpaca::SerializeOrder(request);

    ASSERT_FALSE(encoded);
    EXPECT_EQ(encoded.error(),
              "take_profit.limit_price: must use $0.01 increments at or above $1.00 and $0.0001 below $1.00");
}

TEST(AlpacaOrderCodec, SerializesReplacementFieldsOnly) {
    const core::ReplaceOrderRequest request{
        .qty = D("10"),
        .limit_price = D("199.50"),
        .client_order_id = "tb-replace-1",
    };

    const auto encoded = broker::alpaca::SerializeReplacement(request);

    ASSERT_TRUE(encoded);
    const auto json = nlohmann::json::parse(*encoded);
    EXPECT_EQ(json.size(), 3U);
    EXPECT_EQ(json.at("qty"), "10");
    EXPECT_EQ(json.at("limit_price"), "199.5");
}

}  // namespace
