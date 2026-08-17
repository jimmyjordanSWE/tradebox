#include "buying_power_display.h"

#include <gtest/gtest.h>

#include <string_view>

namespace tradebox::gui {
namespace {

core::Decimal D(std::string_view value) {
    return *core::Decimal::Parse(value);
}

TEST(BuyingPowerDisplay, DoesNotInventPercentageFromDynamicBrokerFields) {
    const core::AccountState account{
        .multiplier = "1",
        .equity = D("100"),
        .buying_power = D("200"),
        .non_marginable_buying_power = D("75"),
        .regt_buying_power = D("150"),
    };

    const BuyingPowerDisplay display = BuildBuyingPowerDisplay(account);
    EXPECT_EQ(display.summary, "BP $200 available");
    EXPECT_EQ(display.summary.find('%'), std::string::npos);
    EXPECT_NE(display.tooltip.find("Broker-reported buying power: $200.00"),
              std::string::npos);
    EXPECT_NE(display.tooltip.find("Reg T buying power: $150.00"),
              std::string::npos);
    EXPECT_NE(display.tooltip.find("Account multiplier: 1x"),
              std::string::npos);
    EXPECT_NE(display.tooltip.find("trading session"), std::string::npos);
}

}  // namespace
}  // namespace tradebox::gui
