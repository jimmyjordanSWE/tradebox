#include "tradebox/application/indicator_projection_cache.h"
#include "tradebox/application/market_data_interest.h"

#include <gtest/gtest.h>

namespace tradebox::application {
namespace {

TEST(MarketDataInterestCoordinator, UnionsConsumersAndAdmitsCriticalFirst) {
    MarketDataInterestCoordinator coordinator(2);
    auto visible = coordinator.Upsert({
        .consumer_id = "watch-list:1",
        .symbols = {"MSFT", "AAPL", "AAPL"},
        .channels = {.trades = false, .quotes = true, .statuses = false},
        .priority = MarketDataInterestPriority::UserVisible,
    });
    ASSERT_TRUE(visible) << visible.error();
    EXPECT_EQ(visible->Symbols(),
              (std::vector<std::string>{"AAPL", "MSFT"}));

    auto critical = coordinator.Upsert({
        .consumer_id = "position-risk",
        .symbols = {"NVDA"},
        .channels = {.trades = true, .quotes = false, .statuses = true},
        .priority = MarketDataInterestPriority::TradingCritical,
    });
    ASSERT_TRUE(critical) << critical.error();
    EXPECT_EQ(critical->Symbols(),
              (std::vector<std::string>{"AAPL", "NVDA"}));
    ASSERT_EQ(critical->rejected.size(), 1U);
    EXPECT_EQ(critical->rejected.front().symbol, "MSFT");
    EXPECT_EQ(critical->rejected.front().reason,
              "Market-data subscription capacity reached");

    auto shared = coordinator.Upsert({
        .consumer_id = "chart:1",
        .symbols = {"NVDA"},
        .channels = {.trades = false, .quotes = true, .statuses = false},
        .priority = MarketDataInterestPriority::UserVisible,
    });
    ASSERT_TRUE(shared) << shared.error();
    const auto nvda = std::ranges::find(
        shared->accepted, "NVDA",
        &EffectiveMarketDataSubscription::symbol);
    ASSERT_NE(nvda, shared->accepted.end());
    EXPECT_TRUE(nvda->channels.trades);
    EXPECT_TRUE(nvda->channels.quotes);
    EXPECT_TRUE(nvda->channels.statuses);
    EXPECT_EQ(nvda->consumers,
              (std::vector<std::string>{"chart:1", "position-risk"}));
}

TEST(MarketDataInterestCoordinator, RemovalReleasesCapacityPerFeed) {
    MarketDataInterestCoordinator coordinator(1);
    ASSERT_TRUE(coordinator.Upsert({
        .consumer_id = "watch-list:1",
        .feed = core::MarketDataFeed::Iex,
        .symbols = {"AAPL"},
    }));
    ASSERT_TRUE(coordinator.Upsert({
        .consumer_id = "sip-chart",
        .feed = core::MarketDataFeed::Sip,
        .symbols = {"MSFT"},
    }));
    EXPECT_EQ(coordinator.Plan(core::MarketDataFeed::Iex).Symbols(),
              (std::vector<std::string>{"AAPL"}));
    EXPECT_EQ(coordinator.Plan(core::MarketDataFeed::Sip).Symbols(),
              (std::vector<std::string>{"MSFT"}));
    EXPECT_TRUE(coordinator.Remove("watch-list:1"));
    EXPECT_TRUE(
        coordinator.Plan(core::MarketDataFeed::Iex).accepted.empty());
}

TEST(IndicatorProjectionCache, SharesExactGraphAtOneInputRevision) {
    const auto one = core::Decimal::Parse("1");
    ASSERT_TRUE(one);
    core::BarSeriesSnapshot series{
        .key = {.instrument_id = "asset:aapl",
                .feed = core::MarketDataFeed::Iex,
                .timeframe = "1Min"},
        .requested_range = {0, 100},
        .bars = {{.start_ns = 1,
                  .open = *one,
                  .high = *one,
                  .low = *one,
                  .close = *one,
                  .volume = *one}},
        .revision = 7,
    };
    const std::vector<core::IndicatorDefinition> definitions{
        {.id = "sma",
         .calculation = core::SimpleMovingAverageCalculation{.period = 1}},
    };
    IndicatorProjectionCache cache;
    const auto first = cache.Resolve(series, definitions);
    const auto second = cache.Resolve(series, definitions);
    EXPECT_EQ(first.get(), second.get());
    ASSERT_EQ(first->series.size(), 1U);

    series.revision = 8;
    const auto revised = cache.Resolve(series, definitions);
    EXPECT_NE(first.get(), revised.get());
}

}  // namespace
}  // namespace tradebox::application
