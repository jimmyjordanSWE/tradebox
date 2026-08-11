#include "tradebox/broker/alpaca_history_scheduler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <vector>

namespace {

using tradebox::broker::alpaca::HistoricalWork;
using tradebox::broker::alpaca::HistoricalWorkKind;
using tradebox::broker::alpaca::HistoricalWorkPriority;
using tradebox::broker::alpaca::HistoricalWorkScheduler;
using tradebox::broker::alpaca::HistoricalWorkSubmission;

TEST(HistoricalWorkScheduler, CoalescesStableKeysWhileWorkIsQueuedOrActive) {
    HistoricalWorkScheduler scheduler{1, 4};
    std::promise<void> started;
    std::promise<void> release;
    const std::shared_future<void> release_signal = release.get_future().share();
    std::atomic<int> executed = 0;
    ASSERT_EQ(scheduler.Submit({
                  .kind = HistoricalWorkKind::Ticks,
                  .priority = HistoricalWorkPriority::Interactive,
                  .key = "tick|PLTR",
                  .execute = [&] {
                      ++executed;
                      started.set_value();
                      release_signal.wait();
                  },
              }),
              HistoricalWorkSubmission::Accepted);
    started.get_future().wait();
    EXPECT_EQ(scheduler.Submit({
                  .kind = HistoricalWorkKind::Ticks,
                  .priority = HistoricalWorkPriority::Interactive,
                  .key = "tick|PLTR",
                  .execute = [] {},
              }),
              HistoricalWorkSubmission::Coalesced);
    release.set_value();
    scheduler.WaitForIdle();
    EXPECT_EQ(executed, 1);
    EXPECT_EQ(scheduler.Health().coalesced, 1U);
}

TEST(HistoricalWorkScheduler, RejectsWhenTheBoundedQueueIsFull) {
    HistoricalWorkScheduler scheduler{1, 1};
    std::promise<void> started;
    std::promise<void> release;
    const std::shared_future<void> release_signal = release.get_future().share();
    ASSERT_EQ(scheduler.Submit({
                  .key = "running",
                  .coalesce = false,
                  .execute = [&] {
                      started.set_value();
                      release_signal.wait();
                  },
              }),
              HistoricalWorkSubmission::Accepted);
    started.get_future().wait();
    ASSERT_EQ(scheduler.Submit({.key = "queued", .execute = [] {}}),
              HistoricalWorkSubmission::Accepted);
    EXPECT_EQ(scheduler.Submit({.key = "rejected", .execute = [] {}}),
              HistoricalWorkSubmission::Rejected);
    release.set_value();
    scheduler.WaitForIdle();
    EXPECT_EQ(scheduler.Health().rejected, 1U);
}

TEST(HistoricalWorkScheduler, RunsInteractiveWorkAheadOfQueuedRecoveryWork) {
    HistoricalWorkScheduler scheduler{1, 4};
    std::promise<void> started;
    std::promise<void> release;
    const std::shared_future<void> release_signal = release.get_future().share();
    std::mutex order_mutex;
    std::vector<std::string> order;
    ASSERT_EQ(scheduler.Submit({
                  .key = "running",
                  .coalesce = false,
                  .execute = [&] {
                      started.set_value();
                      release_signal.wait();
                  },
              }),
              HistoricalWorkSubmission::Accepted);
    started.get_future().wait();
    ASSERT_EQ(scheduler.Submit({
                  .priority = HistoricalWorkPriority::Recovery,
                  .key = "recovery",
                  .execute = [&] {
                      std::scoped_lock lock(order_mutex);
                      order.push_back("recovery");
                  },
              }),
              HistoricalWorkSubmission::Accepted);
    ASSERT_EQ(scheduler.Submit({
                  .priority = HistoricalWorkPriority::Interactive,
                  .key = "interactive",
                  .execute = [&] {
                      std::scoped_lock lock(order_mutex);
                      order.push_back("interactive");
                  },
              }),
              HistoricalWorkSubmission::Accepted);
    release.set_value();
    scheduler.WaitForIdle();
    ASSERT_EQ(order.size(), 2U);
    EXPECT_EQ(order[0], "interactive");
    EXPECT_EQ(order[1], "recovery");
}

TEST(HistoricalWorkScheduler, CancelsPendingWorkWithoutCancelingActiveWork) {
    HistoricalWorkScheduler scheduler{1, 4};
    std::promise<void> started;
    std::promise<void> release;
    const std::shared_future<void> release_signal = release.get_future().share();
    std::atomic<int> canceled = 0;
    ASSERT_EQ(scheduler.Submit({
                  .key = "running",
                  .coalesce = false,
                  .execute = [&] {
                      started.set_value();
                      release_signal.wait();
                  },
              }),
              HistoricalWorkSubmission::Accepted);
    started.get_future().wait();
    ASSERT_EQ(scheduler.Submit({
                  .key = "pending",
                  .execute = [] {},
                  .canceled = [&] { ++canceled; },
              }),
              HistoricalWorkSubmission::Accepted);
    scheduler.CancelPending();
    EXPECT_EQ(canceled, 1);
    release.set_value();
    scheduler.WaitForIdle();
    EXPECT_EQ(scheduler.Health().canceled, 1U);
}

}  // namespace
