#include "tradebox/broker/alpaca_history_scheduler.h"

#include <algorithm>
#include <utility>

namespace tradebox::broker::alpaca {
namespace {

std::deque<HistoricalWork>& QueueFor(
    HistoricalWorkPriority priority,
    std::deque<HistoricalWork>& interactive,
    std::deque<HistoricalWork>& visible,
    std::deque<HistoricalWork>& recovery) {
    switch (priority) {
        case HistoricalWorkPriority::Interactive: return interactive;
        case HistoricalWorkPriority::Visible: return visible;
        case HistoricalWorkPriority::Recovery: return recovery;
    }
    return recovery;
}

}  // namespace

HistoricalWorkScheduler::HistoricalWorkScheduler(std::size_t worker_count,
                                                 std::size_t queue_capacity)
    : queue_capacity_(std::max<std::size_t>(1, queue_capacity)) {
    const std::size_t workers = std::clamp<std::size_t>(worker_count, 1, 8);
    workers_.reserve(workers);
    for (std::size_t index = 0; index < workers; ++index)
        workers_.emplace_back(&HistoricalWorkScheduler::WorkerLoop, this);
}

HistoricalWorkScheduler::~HistoricalWorkScheduler() { Stop(); }

HistoricalWorkSubmission HistoricalWorkScheduler::Submit(HistoricalWork work) {
    if (work.key.empty() || !work.execute) return HistoricalWorkSubmission::Rejected;
    {
        std::scoped_lock lock(mutex_);
        if (stopping_ || health_.queued >= queue_capacity_) {
            ++health_.rejected;
            return HistoricalWorkSubmission::Rejected;
        }
        if (work.coalesce && active_keys_.contains(work.key)) {
            ++health_.coalesced;
            return HistoricalWorkSubmission::Coalesced;
        }
        if (work.coalesce) active_keys_.insert(work.key);
        QueueFor(work.priority, interactive_, visible_, recovery_)
            .push_back(std::move(work));
        ++health_.queued;
        health_.queue_high_water =
            std::max(health_.queue_high_water, health_.queued);
    }
    ready_.notify_one();
    return HistoricalWorkSubmission::Accepted;
}

void HistoricalWorkScheduler::CancelPending() {
    std::vector<HistoricalWork> canceled;
    {
        std::scoped_lock lock(mutex_);
        const auto drain = [&](std::deque<HistoricalWork>& queue) {
            while (!queue.empty()) {
                HistoricalWork work = std::move(queue.front());
                queue.pop_front();
                if (work.coalesce) active_keys_.erase(work.key);
                canceled.push_back(std::move(work));
                --health_.queued;
                ++health_.canceled;
            }
        };
        drain(interactive_);
        drain(visible_);
        drain(recovery_);
        if (EmptyLocked()) idle_.notify_all();
    }
    for (HistoricalWork& work : canceled)
        if (work.canceled) work.canceled();
    ready_.notify_all();
}

void HistoricalWorkScheduler::WaitForIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return EmptyLocked(); });
}

void HistoricalWorkScheduler::Stop() {
    CancelPending();
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        health_.stopping = true;
    }
    ready_.notify_all();
    for (std::thread& worker : workers_)
        if (worker.joinable()) worker.join();
}

HistoricalWorkHealth HistoricalWorkScheduler::Health() const {
    std::scoped_lock lock(mutex_);
    return health_;
}

bool HistoricalWorkScheduler::EmptyLocked() const {
    return health_.queued == 0 && health_.in_flight == 0;
}

void HistoricalWorkScheduler::WorkerLoop() {
    for (;;) {
        HistoricalWork work;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] {
                return stopping_ || health_.queued != 0;
            });
            if (stopping_ && health_.queued == 0) return;
            auto take = [&](std::deque<HistoricalWork>& queue) {
                work = std::move(queue.front());
                queue.pop_front();
            };
            if (!interactive_.empty()) take(interactive_);
            else if (!visible_.empty()) take(visible_);
            else take(recovery_);
            --health_.queued;
            ++health_.in_flight;
        }
        try {
            work.execute();
        } catch (...) {
            // The owning broker request supplies typed failure reporting.
            // Scheduler state must still be released for subsequent work.
        }
        {
            std::scoped_lock lock(mutex_);
            if (work.coalesce) active_keys_.erase(work.key);
            --health_.in_flight;
            ++health_.completed;
            if (EmptyLocked()) idle_.notify_all();
        }
        ready_.notify_all();
    }
}

}  // namespace tradebox::broker::alpaca
