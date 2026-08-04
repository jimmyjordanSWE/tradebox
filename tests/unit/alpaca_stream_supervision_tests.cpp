#include "tradebox/broker/alpaca_stream_supervision.h"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using tradebox::broker::alpaca::ReconnectBackoff;
using tradebox::broker::alpaca::StreamChannel;
using tradebox::broker::alpaca::StreamSupervisionPolicy;
using tradebox::broker::alpaca::SubscriptionRecovery;

TEST(AlpacaStreamSupervision,
     RepeatedFailuresBackOffWithinBoundWithDeterministicJitter) {
    const StreamSupervisionPolicy policy{
        .initial_retry = 1s,
        .maximum_retry = 8s,
        .stable_session = 30s,
    };
    ReconnectBackoff first(StreamChannel::MarketData, policy);
    ReconnectBackoff second(StreamChannel::MarketData, policy);

    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto first_delay = first.NextDelay(false, 0ms);
        const auto second_delay = second.NextDelay(false, 0ms);
        EXPECT_EQ(first_delay, second_delay);
        EXPECT_GE(first_delay, 800ms);
        EXPECT_LE(first_delay, 8s);
    }
}

TEST(AlpacaStreamSupervision,
     StableReadySessionResetsFailureEscalation) {
    const StreamSupervisionPolicy policy{
        .initial_retry = 1s,
        .maximum_retry = 30s,
        .stable_session = 10s,
    };
    ReconnectBackoff backoff(StreamChannel::Account, policy);
    static_cast<void>(backoff.NextDelay(false, 0ms));
    static_cast<void>(backoff.NextDelay(false, 0ms));
    EXPECT_EQ(backoff.failure_count(), 2U);

    const auto reset_delay = backoff.NextDelay(true, 10s);
    EXPECT_EQ(backoff.failure_count(), 1U);
    EXPECT_GE(reset_delay, 800ms);
    EXPECT_LE(reset_delay, 1200ms);
}

TEST(AlpacaStreamSupervision,
     BackfillRequiresAtLeastOneCompletedDisconnectedMinute) {
    constexpr std::int64_t minute = 60LL * 1'000'000'000;
    EXPECT_FALSE(
        tradebox::broker::alpaca::HasRecoverableMinuteGap(
            10 * minute + 5, 10 * minute + 30));
    EXPECT_TRUE(
        tradebox::broker::alpaca::HasRecoverableMinuteGap(
            10 * minute + 5, 11 * minute + 30));
}

TEST(AlpacaStreamSupervision,
     SubscriptionMustExactlyMatchTradesAndQuotes) {
    using tradebox::broker::alpaca::EvaluateSubscription;
    const std::vector<std::string> desired{"MSFT", "AMD", "AMD"};
    EXPECT_EQ(
        EvaluateSubscription(
            desired, {"AMD", "MSFT"}, {"MSFT", "AMD"},
            {"*"}, 0),
        SubscriptionRecovery::Ready);
    EXPECT_EQ(
        EvaluateSubscription(
            desired, {"AMD"}, {"AMD", "MSFT"}, {"*"}, 1),
        SubscriptionRecovery::Repair);
    EXPECT_EQ(
        EvaluateSubscription(
            desired, {"AMD", "MSFT"}, {"AMD", "MSFT"}, {}, 3),
        SubscriptionRecovery::Ready);
    EXPECT_EQ(
        EvaluateSubscription(
            desired, {"AMD"}, {"AMD"}, {}, 3),
        SubscriptionRecovery::Restart);
}

TEST(AlpacaStreamSupervision,
     ShutdownCancelsEvenAMaximumReconnectDelayImmediately) {
    const std::atomic<bool> running = false;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(
        tradebox::broker::alpaca::InterruptibleReconnectWait(
            running, 30s));
    EXPECT_LT(
        std::chrono::steady_clock::now() - started, 20ms);
}

TEST(AlpacaStreamSupervision,
     SilenceSupervisionUsesTheWinHttpKeepaliveFloor) {
    const StreamSupervisionPolicy policy;
    EXPECT_EQ(policy.keepalive_interval, 15s);
}

}  // namespace
