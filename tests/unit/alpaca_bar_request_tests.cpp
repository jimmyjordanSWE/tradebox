#include "tradebox/broker/alpaca_bar_request.h"
#include "tradebox/broker/alpaca_page_validation.h"

#include <gtest/gtest.h>

namespace {

using namespace tradebox;

core::BarSeriesKey Key() {
    return {
        .instrument_id = "alpaca:asset-aapl",
        .feed = core::MarketDataFeed::Sip,
        .timeframe = "1Min",
        .adjustment = core::BarAdjustment::All,
    };
}

TEST(AlpacaHistoricalPage,
     RejectsMalformedShapesInsteadOfClaimingCoverage) {
    using tradebox::broker::alpaca::ValidateHistoricalPage;
    EXPECT_FALSE(ValidateHistoricalPage("[]", "bars"));
    EXPECT_FALSE(ValidateHistoricalPage(
        R"({"next_page_token":null})", "bars"));
    EXPECT_FALSE(ValidateHistoricalPage(
        R"({"bars":{},"next_page_token":null})",
        "bars"));
    EXPECT_FALSE(ValidateHistoricalPage(
        R"({"bars":[],"next_page_token":42})",
        "bars"));
    EXPECT_FALSE(ValidateHistoricalPage(
        R"({"bars":[1],"next_page_token":null})",
        "bars"));
    const auto provider_error = ValidateHistoricalPage(
        R"({"message":"insufficient subscription"})", "trades");
    ASSERT_FALSE(provider_error);
    EXPECT_NE(provider_error.error().find("insufficient subscription"),
              std::string::npos);

    const auto valid = ValidateHistoricalPage(
        R"({"bars":[],"next_page_token":"next"})",
        "bars");
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid->next_page_token, "next");
}

TEST(AlpacaBarRequest,
     BuildsFeedQualifiedHalfOpenPaginatedPath) {
    const std::string path =
        broker::alpaca::BuildHistoricalBarPath({
            .symbol = "BRK/B",
            .key = Key(),
            .range = {0, 1'000'000'000},
            .page_token = "next/+",
            .limit = 50'000,
        });

    EXPECT_EQ(
        path,
        "/v2/stocks/BRK%2FB/bars?"
        "timeframe=1Min&"
        "start=1970-01-01T00%3A00%3A00Z&"
        "end=1970-01-01T00%3A00%3A00.999999999Z&"
        "limit=10000&adjustment=all&feed=sip&sort=asc&"
        "page_token=next%2F%2B");
}

TEST(AlpacaBarRequest, RejectsAnEmptyOrInvalidRange) {
    auto request = broker::alpaca::HistoricalBarPageRequest{
        .symbol = "AAPL",
        .key = Key(),
        .range = {100, 100},
    };
    EXPECT_TRUE(
        broker::alpaca::BuildHistoricalBarPath(request).empty());
    request.range = {101, 100};
    EXPECT_TRUE(
        broker::alpaca::BuildHistoricalBarPath(request).empty());
}

TEST(AlpacaBarRequest,
     ReservesOnlyRangesNotAlreadyBeingFetched) {
    broker::alpaca::InFlightBarRanges in_flight;
    const auto first =
        in_flight.Reserve(Key(), {{0, 100}});
    EXPECT_EQ(first,
              (std::vector<core::BarRange>{{0, 100}}));

    const auto overlap =
        in_flight.Reserve(Key(), {{50, 150}});
    EXPECT_EQ(overlap,
              (std::vector<core::BarRange>{{100, 150}}));

    in_flight.Release(Key(), first);
    const auto after_release =
        in_flight.Reserve(Key(), {{0, 125}});
    EXPECT_EQ(after_release,
              (std::vector<core::BarRange>{{0, 100}}));
}

TEST(BarRanges, SubtractsMergedCoverageFromRequests) {
    std::vector<core::BarRange> covered{{100, 200}};
    core::MergeBarRange(covered, {200, 250});
    core::MergeBarRange(covered, {300, 350});
    EXPECT_EQ(
        core::MissingBarRanges(covered, {50, 400}),
        (std::vector<core::BarRange>{
            {50, 100}, {250, 300}, {350, 400}}));
    EXPECT_EQ(
        core::SubtractBarRanges(
            {{0, 500}}, {{100, 200}, {300, 400}}),
        (std::vector<core::BarRange>{
            {0, 100}, {200, 300}, {400, 500}}));
}

}  // namespace
