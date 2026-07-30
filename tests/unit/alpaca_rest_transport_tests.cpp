#include "tradebox/broker/alpaca_rest_transport.h"
#include "tradebox/broker/alpaca_auth.h"
#include "tradebox/broker/alpaca_payload_limits.h"
#include "tradebox/platform/credentials.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using namespace tradebox::broker::alpaca;
using namespace std::chrono_literals;

static_assert(!std::is_copy_constructible_v<AlpacaCredentials>);
static_assert(!std::is_copy_assignable_v<AlpacaCredentials>);
static_assert(std::is_move_constructible_v<AlpacaCredentials>);
static_assert(std::is_move_assignable_v<AlpacaCredentials>);

TEST(AlpacaInboundPayload, EnforcesTheLimitWithoutPartialAppend) {
    std::string payload = "1234";
    EXPECT_TRUE(AppendInboundPayload(payload, "56", 6));
    EXPECT_EQ(payload, "123456");
    EXPECT_FALSE(AppendInboundPayload(payload, "7", 6));
    EXPECT_EQ(payload, "123456");
}

TEST(AlpacaAuthentication, EscapesCredentialJsonWithoutAJsonObjectCopy) {
    const std::string message =
        BuildAuthenticationMessage(
            "key\"line", "secret\\\nvalue");
    const std::string expected =
        "{\"action\":\"auth\",\"key\":\"key\\\"line\","
        "\"secret\":\"secret\\\\\\nvalue\"}";
    EXPECT_EQ(message, expected);
}

class FakeExecutor final : public IRestExecutor {
public:
    RestResponse Execute(const RestRequest& request) override {
        std::unique_lock lock(mutex);
        calls.push_back(request);
        started.notify_all();
        if (block_first && calls.size() == 1)
            release.wait(lock, [this] { return released; });
        if (responses.empty()) return {.status = 200};
        RestResponse response = std::move(responses.front());
        responses.pop_front();
        return response;
    }

    void WaitForCalls(std::size_t count) {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(started.wait_for(
            lock, 2s, [this, count] { return calls.size() >= count; }));
    }

    void Release() {
        {
            std::scoped_lock lock(mutex);
            released = true;
        }
        release.notify_all();
    }

    std::mutex mutex;
    std::condition_variable started;
    std::condition_variable release;
    std::vector<RestRequest> calls;
    std::deque<RestResponse> responses;
    bool block_first = false;
    bool released = false;
};

RestRequest Read(
    std::string path,
    RestPriority priority = RestPriority::Background,
    RestDomain domain = RestDomain::Trading) {
    return {
        .method = "GET",
        .host = domain == RestDomain::Trading
                    ? "paper-api.alpaca.markets"
                    : "data.alpaca.markets",
        .path = std::move(path),
        .api_key = "key",
        .api_secret = "secret",
        .domain = domain,
        .priority = priority,
    };
}

TEST(AlpacaRestTransport, CommandsOvertakeQueuedBackgroundReads) {
    auto executor = std::make_unique<FakeExecutor>();
    FakeExecutor* fake = executor.get();
    fake->block_first = true;
    AlpacaRestTransport transport(std::move(executor), 1, 10);

    auto running = transport.Submit(Read("/running"));
    fake->WaitForCalls(1);
    auto background = transport.Submit(Read("/background"));
    auto command = transport.Submit({
        .method = "DELETE",
        .host = "paper-api.alpaca.markets",
        .path = "/v2/orders",
        .priority = RestPriority::Command,
        .safe_to_retry = false,
        .coalesce = false,
    });
    fake->Release();
    ASSERT_EQ(command.get().status, 200U);
    ASSERT_EQ(background.get().status, 200U);
    ASSERT_EQ(running.get().status, 200U);

    std::scoped_lock lock(fake->mutex);
    ASSERT_EQ(fake->calls.size(), 3U);
    EXPECT_EQ(fake->calls[1].path, "/v2/orders");
    EXPECT_EQ(fake->calls[2].path, "/background");
}

TEST(AlpacaRestTransport, ReservesConcurrencyForEmergencyCommands) {
    auto executor = std::make_unique<FakeExecutor>();
    FakeExecutor* fake = executor.get();
    fake->block_first = true;
    AlpacaRestTransport transport(std::move(executor), 2, 10);

    auto first = transport.Submit(Read("/background-one"));
    fake->WaitForCalls(1);
    auto second = transport.Submit(Read("/background-two"));
    auto command = transport.Submit({
        .method = "DELETE",
        .host = "paper-api.alpaca.markets",
        .path = "/v2/orders",
        .priority = RestPriority::Command,
        .safe_to_retry = false,
        .coalesce = false,
    });
    EXPECT_EQ(command.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(command.get().status, 200U);
    fake->Release();
    EXPECT_EQ(first.get().status, 200U);
    EXPECT_EQ(second.get().status, 200U);

    std::scoped_lock lock(fake->mutex);
    ASSERT_EQ(fake->calls.size(), 3U);
    EXPECT_EQ(fake->calls[1].path, "/v2/orders");
}

TEST(AlpacaRestTransport, CoalescesIdenticalSafeReads) {
    auto executor = std::make_unique<FakeExecutor>();
    FakeExecutor* fake = executor.get();
    fake->block_first = true;
    AlpacaRestTransport transport(std::move(executor), 1, 10);

    auto first = transport.Submit(Read("/v2/account"));
    fake->WaitForCalls(1);
    auto duplicate = transport.Submit(Read("/v2/account"));
    fake->Release();

    EXPECT_EQ(first.get().status, 200U);
    EXPECT_EQ(duplicate.get().status, 200U);
    EXPECT_EQ(transport.Health().coalesced, 1U);
    std::scoped_lock lock(fake->mutex);
    EXPECT_EQ(fake->calls.size(), 1U);
}

TEST(AlpacaRestTransport,
     RetriesSafe429ButNeverRetriesAmbiguousWrites) {
    {
        auto executor = std::make_unique<FakeExecutor>();
        FakeExecutor* fake = executor.get();
        fake->responses = {
            {.status = 429,
             .rate_limit = 200,
             .rate_remaining = 10},
            {.status = 200,
             .rate_limit = 200,
             .rate_remaining = 9},
        };
        AlpacaRestTransport transport(std::move(executor), 1, 10);
        EXPECT_EQ(transport.Execute(Read("/safe")).status, 200U);
        EXPECT_EQ(transport.Health().retries, 1U);
        EXPECT_EQ(transport.Health().trading.remaining, 9);
        std::scoped_lock lock(fake->mutex);
        EXPECT_EQ(fake->calls.size(), 2U);
    }
    {
        auto executor = std::make_unique<FakeExecutor>();
        FakeExecutor* fake = executor.get();
        fake->responses = {{.status = 500}};
        AlpacaRestTransport transport(std::move(executor), 1, 10);
        const auto response = transport.Execute({
            .method = "POST",
            .host = "paper-api.alpaca.markets",
            .path = "/v2/orders",
            .priority = RestPriority::Command,
            .safe_to_retry = false,
            .coalesce = false,
        });
        EXPECT_EQ(response.status, 500U);
        EXPECT_EQ(transport.Health().retries, 0U);
        std::scoped_lock lock(fake->mutex);
        EXPECT_EQ(fake->calls.size(), 1U);
    }
}

TEST(AlpacaRestTransport,
     TracksSeparateBudgetsAndDefersAnExhaustedDomain) {
    auto executor = std::make_unique<FakeExecutor>();
    FakeExecutor* fake = executor.get();
    const auto reset =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count() +
        2;
    fake->responses = {
        {.status = 200,
         .rate_limit = 200,
         .rate_remaining = 0,
         .rate_reset_epoch_seconds = reset},
        {.status = 200,
         .rate_limit = 10'000,
         .rate_remaining = 9'999},
    };
    AlpacaRestTransport transport(std::move(executor), 1, 10);
    EXPECT_EQ(transport.Execute(Read("/account")).status, 200U);
    EXPECT_EQ(
        transport
            .Execute(Read(
                "/bars", RestPriority::Interactive,
                RestDomain::MarketData))
            .status,
        200U);
    auto deferred = transport.Submit(Read("/positions"));
    EXPECT_EQ(deferred.wait_for(50ms), std::future_status::timeout);
    const auto health = transport.Health();
    EXPECT_EQ(health.trading.remaining, 0);
    EXPECT_EQ(health.market_data.remaining, 9'999);
    transport.CancelPending();
    EXPECT_EQ(deferred.get().error, "REST request canceled");
}

TEST(AlpacaRestTransport, BoundsQueueAndCancelsPendingOnShutdown) {
    auto executor = std::make_unique<FakeExecutor>();
    FakeExecutor* fake = executor.get();
    fake->block_first = true;
    auto transport = std::make_unique<AlpacaRestTransport>(
        std::move(executor), 1, 1);
    auto running = transport->Submit(Read("/running"));
    fake->WaitForCalls(1);
    auto pending = transport->Submit(Read("/pending"));
    auto rejected = transport->Submit(Read("/overflow"));
    EXPECT_EQ(rejected.get().error, "REST request queue is full");
    EXPECT_EQ(transport->Health().rejected, 1U);

    auto shutdown = std::async(
        std::launch::async,
        [owned = std::move(transport)]() mutable { owned.reset(); });
    EXPECT_EQ(pending.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(pending.get().error, "REST transport is shutting down");
    fake->Release();
    EXPECT_EQ(running.get().error, "REST request canceled");
    EXPECT_EQ(shutdown.wait_for(2s), std::future_status::ready);
}

}  // namespace
