#include "tradebox/broker/alpaca_account_activity.h"

#include <gtest/gtest.h>

namespace {

using namespace tradebox;

core::OrderState FilledOrder() {
    core::OrderState order;
    order.id = "order-1";
    order.filled_qty = *core::Decimal::Parse("3");
    return order;
}

TEST(AlpacaAccountActivity,
     BuildsEncodedIncrementalPaginatedTradingApiPath) {
    EXPECT_EQ(
        broker::alpaca::BuildAccountActivitiesPath(
            "token::one", "2026-07-29T12:00:00.123Z"),
        "/v2/account/activities?direction=desc&page_size=100"
        "&page_token=token%3A%3Aone"
        "&after=2026-07-29T12%3A00%3A00.123Z");
}

TEST(AlpacaAccountActivity,
     ParsesLegacyAndCurrentActivitiesWithExactValues) {
    const std::string payload = R"([
      {
        "id":"fill-1","activity_type":"FILL","order_id":"order-1",
        "symbol":"AAPL","side":"buy","type":"fill",
        "qty":"1.25","cum_qty":"3","leaves_qty":"0",
        "price":"201.123456789",
        "transaction_time":"2026-07-29T12:00:00.123456Z"
      },
      {
        "event_id":"fee-1","ref_id":"ref-fee","account_id":"account-1",
        "activity_type":"FEE","activity_subtype":"REG",
        "net_amount":"-0.01","currency":"USD",
        "at":"2026-07-29T12:01:00Z",
        "details":{"symbol":"AAPL"}
      }
    ])";

    const auto page = broker::alpaca::ParseAccountActivityPage(
        payload, "account-1", {FilledOrder()}, {}, 2);

    ASSERT_TRUE(page);
    ASSERT_EQ(page->activities.size(), 2U);
    EXPECT_EQ(page->activities[0].price->ToString(),
              "201.123456789");
    EXPECT_EQ(
        page->activities[0].fill_reconciliation,
        core::ActivityFillReconciliation::MatchedOrder);
    EXPECT_EQ(page->activities[1].activity_subtype, "REG");
    EXPECT_EQ(page->activities[1].net_amount->ToString(), "-0.01");
    EXPECT_EQ(page->next_page_token, "fee-1");
}

TEST(AlpacaAccountActivity,
     ReportsMissingAndQuantityMismatchedFillLinksWithoutCreatingFills) {
    core::OrderState short_order = FilledOrder();
    short_order.filled_qty = *core::Decimal::Parse("1");
    const auto page = broker::alpaca::ParseAccountActivityPage(
        R"([
          {"id":"a","activity_type":"FILL","order_id":"missing",
           "qty":"1","transaction_time":"2026-07-29T12:00:00Z"},
          {"id":"b","activity_type":"FILL","order_id":"order-1",
           "cum_qty":"2","transaction_time":"2026-07-29T12:00:01Z"}
        ])",
        "account-1", {short_order}, {}, 100);

    ASSERT_TRUE(page);
    EXPECT_EQ(
        page->activities[0].fill_reconciliation,
        core::ActivityFillReconciliation::OrderNotFound);
    EXPECT_EQ(
        page->activities[1].fill_reconciliation,
        core::ActivityFillReconciliation::QuantityMismatch);
}

TEST(AlpacaAccountActivity, RejectsRepeatedPaginationToken) {
    const auto page = broker::alpaca::ParseAccountActivityPage(
        R"([{"id":"same","activity_type":"DIV","date":"2026-07-29"}])",
        "account-1", {}, "same", 1);

    ASSERT_FALSE(page);
    EXPECT_NE(page.error().find("repeated"), std::string::npos);
}

}  // namespace
