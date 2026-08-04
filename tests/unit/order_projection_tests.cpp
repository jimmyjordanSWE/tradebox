#include "tradebox/core/order_projection.h"

#include <gtest/gtest.h>

using tradebox::core::OrderLifecycleState;
using tradebox::core::OrderState;
using tradebox::core::ProjectOrder;

TEST(OrderProjection, MapsBrokerLifecycleToTypedState) {
    OrderState order;

    order.status = "pending_cancel";
    EXPECT_EQ(ProjectOrder(order).lifecycle, OrderLifecycleState::Pending);

    order.status = "partially_filled";
    EXPECT_EQ(ProjectOrder(order).lifecycle, OrderLifecycleState::Accepted);

    order.status = "filled";
    EXPECT_EQ(ProjectOrder(order).lifecycle, OrderLifecycleState::Filled);

    order.status = "expired";
    EXPECT_EQ(ProjectOrder(order).lifecycle, OrderLifecycleState::Rejected);
}

TEST(OrderProjection, ExposesCapabilitiesFromLifecycle) {
    OrderState order;
    order.status = "accepted";
    EXPECT_TRUE(ProjectOrder(order).capabilities.cancelable);
    EXPECT_TRUE(ProjectOrder(order).capabilities.replaceable);

    order.status = "canceled";
    EXPECT_FALSE(ProjectOrder(order).capabilities.cancelable);
    EXPECT_FALSE(ProjectOrder(order).capabilities.replaceable);
}

TEST(OrderProjection, ComputesQuickOrderReadModelWithoutUiMath) {
    tradebox::core::AccountState account;
    account.buying_power = *tradebox::core::Decimal::Parse("10000");
    tradebox::core::MarketDataSnapshot market;
    market.symbol = "AMD";
    market.latest_price = tradebox::core::CanonicalMarketPrice{
        .price = *tradebox::core::Decimal::Parse("125.50"),
    };
    tradebox::core::PositionState position;
    position.symbol = "AMD";
    position.side = "long";
    position.qty_available = *tradebox::core::Decimal::Parse("12");

    const auto projection = ProjectQuickOrder(
        account, {position}, market,
        *tradebox::core::Decimal::Parse("100"),
        *tradebox::core::Decimal::Parse("80"));

    EXPECT_EQ(projection.long_budget.ToString(), "10000");
    EXPECT_EQ(projection.short_budget.ToString(), "8000");
    EXPECT_EQ(projection.long_quantity.ToString(), "79");
    EXPECT_EQ(projection.short_quantity.ToString(), "63");
    ASSERT_TRUE(projection.long_exit_quantity.has_value());
    EXPECT_EQ(projection.long_exit_quantity->ToString(), "12");
    EXPECT_FALSE(projection.short_exit_quantity.has_value());
}

TEST(OrderProjection, ComputesBracketPreviewFromTypedInputs) {
    const auto projection = tradebox::core::ProjectBracket(
        *tradebox::core::Decimal::Parse("100"),
        *tradebox::core::Decimal::Parse("10"),
        *tradebox::core::Decimal::Parse("5"),
        *tradebox::core::Decimal::Parse("10"), false);

    EXPECT_EQ(projection.target_price.ToString(), "110");
    EXPECT_EQ(projection.stop_price.ToString(), "95");
    EXPECT_EQ(projection.reward.ToString(), "100");
    EXPECT_EQ(projection.risk.ToString(), "50");
    EXPECT_EQ(projection.risk_reward.ToString(), "2");
}
