#include "tradebox/application/hotkey_order.h"
#include "tradebox/broker/alpaca_price_rules.h"

#include <gtest/gtest.h>

namespace tradebox::application {
namespace {
core::Decimal D(const char* text) { return *core::Decimal::Parse(text); }

TEST(HotkeyOrder, UsesAskForLongAndCapsQuantityAtBuyingPower) {
    core::CoreSnapshot account;
    account.account = core::AccountState{.id = "account", .equity = D("1000"), .buying_power = D("100")};
    core::MarketDataSnapshot market;
    market.latest_quote = core::MarketQuote{.bid_price = D("9"), .ask_price = D("10")};
    const auto preview = BuildHotkeyBracketPreview(account, market,
        {.symbol = "AMD", .side = HotkeyOrderSide::Long, .risk_percent = D("10"), .target_percent = D("10"), .stop_percent = D("10"), .maximum_buying_power_percent = D("100")},
        broker::alpaca::UsEquityPriceIncrementSchedule());
    ASSERT_TRUE(preview);
    EXPECT_EQ(preview->reference_price, D("10"));
    EXPECT_EQ(*preview->order.qty, D("10"));
    EXPECT_TRUE(preview->buying_power_limited);
    EXPECT_EQ(preview->order.take_profit->limit_price, D("11"));
    EXPECT_EQ(preview->order.stop_loss->stop_price, D("9"));
}

TEST(HotkeyOrder, RejectsShortWhenAccountDoesNotAllowIt) {
    core::CoreSnapshot account;
    account.account = core::AccountState{.id = "account", .equity = D("1000"), .buying_power = D("1000")};
    core::MarketDataSnapshot market;
    market.latest_quote = core::MarketQuote{.bid_price = D("10"), .ask_price = D("11")};
    const auto preview = BuildHotkeyBracketPreview(account, market,
        {.symbol = "AMD", .side = HotkeyOrderSide::Short, .risk_percent = D("1"), .target_percent = D("1"), .stop_percent = D("1"), .maximum_buying_power_percent = D("80")},
        broker::alpaca::UsEquityPriceIncrementSchedule());
    ASSERT_FALSE(preview);
    EXPECT_EQ(preview.error(), "Short selling is not enabled for this account");
}

TEST(HotkeyOrder, CapsQuantityAtBuyingPower) {
    core::CoreSnapshot account;
    account.account = core::AccountState{
        .id = "account", .equity = D("1000"), .buying_power = D("2000")};
    core::MarketDataSnapshot market;
    market.latest_quote = core::MarketQuote{.bid_price = D("10"), .ask_price = D("10")};

    const auto preview = BuildHotkeyBracketPreview(
        account, market,
        {.symbol = "AMD", .side = HotkeyOrderSide::Long,
         .risk_percent = D("50"),
         .target_percent = D("10"), .stop_percent = D("1"),
         .maximum_buying_power_percent = D("100")},
        broker::alpaca::UsEquityPriceIncrementSchedule());

    ASSERT_TRUE(preview);
    EXPECT_EQ(*preview->order.qty, D("200"));
    EXPECT_EQ(preview->estimated_entry_notional, D("2000"));
    EXPECT_TRUE(preview->buying_power_limited);
}

TEST(HotkeyOrder, QuantizesGeneratedBracketPricesToAlpacaEquityIncrements) {
    core::CoreSnapshot account;
    account.account = core::AccountState{
        .id = "account", .equity = D("100000"), .buying_power = D("100000")};
    core::MarketDataSnapshot market;
    market.latest_quote = core::MarketQuote{.bid_price = D("309.40"),
                                             .ask_price = D("309.42")};

    const auto preview = BuildHotkeyBracketPreview(
        account, market,
        {.symbol = "AAPL", .side = HotkeyOrderSide::Long,
         .risk_percent = D("1"),
         .target_percent = D("0.80"), .stop_percent = D("0.20"),
         .maximum_buying_power_percent = D("100")},
        broker::alpaca::UsEquityPriceIncrementSchedule());

    ASSERT_TRUE(preview);
    EXPECT_EQ(preview->order.take_profit->limit_price, D("311.89"));
    EXPECT_EQ(preview->order.stop_loss->stop_price, D("308.81"));
}
}  // namespace
}  // namespace tradebox::application
