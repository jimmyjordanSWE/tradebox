#include "tradebox/broker/alpaca_polling_policy.h"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;

TEST(AlpacaPollingPolicy,
     HealthyAuthoritativeSnapshotsUseDefensiveFiveMinuteReanchors) {
    EXPECT_EQ(
        tradebox::broker::alpaca::kAccountReanchorInterval, 5min);
    EXPECT_EQ(
        tradebox::broker::alpaca::kPositionReanchorInterval, 5min);
    EXPECT_EQ(
        tradebox::broker::alpaca::kMarketClockInterval, 5min);
}

TEST(AlpacaPollingPolicy,
     ExceptionalFallbackRemainsFasterThanHealthyPolling) {
    EXPECT_EQ(
        tradebox::broker::alpaca::kOrderFallbackInterval, 5s);
    EXPECT_LT(
        tradebox::broker::alpaca::kOrderFallbackInterval,
        tradebox::broker::alpaca::kAccountReanchorInterval);
}

}  // namespace
