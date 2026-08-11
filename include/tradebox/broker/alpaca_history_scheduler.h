#pragma once

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace tradebox::broker::alpaca {

enum class HistoricalWorkKind { Bars, Ticks, MarketGapBackfill };

enum class HistoricalWorkPriority {
    Interactive,
    Visible,
    Recovery,
};

enum class HistoricalWorkSubmission { Accepted, Coalesced, Rejected };

struct HistoricalWorkHealth {
    std::size_t queued = 0;
    std::size_t in_flight = 0;
    std::size_t queue_high_water = 0;
    std::uint64_t completed = 0;
    std::uint64_t rejected = 0;
    std::uint64_t coalesced = 0;
    std::uint64_t canceled = 0;
    bool stopping = false;
};

struct HistoricalWork {
    HistoricalWorkKind kind = HistoricalWorkKind::Bars;
    HistoricalWorkPriority priority = HistoricalWorkPriority::Visible;
    // The key must be a stable representation of the request identity. Keys
    // are retained while queued and executing so duplicate work is coalesced.
    std::string key;
    bool coalesce = true;
    std::function<void()> execute;
    std::function<void()> canceled;
};

// Owns only broker-adapter scheduling state. Provider rate limits remain the
// responsibility of AlpacaRestTransport, which executes the HTTP requests.
class HistoricalWorkScheduler final {
public:
    explicit HistoricalWorkScheduler(std::size_t worker_count = 2,
                                    std::size_t queue_capacity = 64);
    ~HistoricalWorkScheduler();

    HistoricalWorkScheduler(const HistoricalWorkScheduler&) = delete;
    HistoricalWorkScheduler& operator=(const HistoricalWorkScheduler&) =
        delete;

    [[nodiscard]] HistoricalWorkSubmission Submit(HistoricalWork work);
    void CancelPending();
    void WaitForIdle();
    void Stop();
    [[nodiscard]] HistoricalWorkHealth Health() const;

private:
    void WorkerLoop();
    [[nodiscard]] bool EmptyLocked() const;

    const std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable idle_;
    std::deque<HistoricalWork> interactive_;
    std::deque<HistoricalWork> visible_;
    std::deque<HistoricalWork> recovery_;
    std::unordered_set<std::string> active_keys_;
    std::vector<std::thread> workers_;
    HistoricalWorkHealth health_;
    bool stopping_ = false;
};

}  // namespace tradebox::broker::alpaca
