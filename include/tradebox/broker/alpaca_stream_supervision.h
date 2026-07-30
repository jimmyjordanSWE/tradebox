#pragma once

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tradebox::broker::alpaca {

enum class StreamChannel {
    MarketData,
    Account,
};

struct StreamSupervisionPolicy {
    std::chrono::milliseconds initial_retry{500};
    std::chrono::milliseconds maximum_retry{30'000};
    std::chrono::milliseconds stable_session{30'000};
    std::chrono::milliseconds keepalive_interval{15'000};
};

class ReconnectBackoff {
public:
    explicit ReconnectBackoff(
        StreamChannel channel,
        StreamSupervisionPolicy policy = {});

    [[nodiscard]] std::chrono::milliseconds NextDelay(
        bool reached_ready_state,
        std::chrono::milliseconds session_lifetime);
    void Reset();
    [[nodiscard]] std::size_t failure_count() const {
        return failure_count_;
    }

private:
    StreamChannel channel_;
    StreamSupervisionPolicy policy_;
    std::size_t failure_count_ = 0;
};

enum class SubscriptionRecovery {
    Ready,
    Repair,
    Restart,
};

[[nodiscard]] SubscriptionRecovery EvaluateSubscription(
    const std::vector<std::string>& desired,
    const std::vector<std::string>& acknowledged_trades,
    const std::vector<std::string>& acknowledged_quotes,
    const std::vector<std::string>& acknowledged_statuses,
    std::size_t consecutive_mismatches);

[[nodiscard]] bool InterruptibleReconnectWait(
    const std::atomic<bool>& running,
    std::chrono::milliseconds delay);

[[nodiscard]] bool HasRecoverableMinuteGap(
    std::int64_t disconnected_at_ns,
    std::int64_t reconnected_at_ns);

}  // namespace tradebox::broker::alpaca
