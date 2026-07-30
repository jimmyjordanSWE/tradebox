#include "tradebox/broker/alpaca_stream_supervision.h"

#include <algorithm>
#include <limits>
#include <thread>

namespace tradebox::broker::alpaca {
namespace {

constexpr std::int64_t kMinuteNs = 60LL * 1'000'000'000;

std::uint64_t Mix(std::uint64_t value) {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::vector<std::string> Normalize(
    std::vector<std::string> symbols) {
    std::ranges::sort(symbols);
    symbols.erase(
        std::unique(symbols.begin(), symbols.end()),
        symbols.end());
    return symbols;
}

}  // namespace

ReconnectBackoff::ReconnectBackoff(
    StreamChannel channel,
    StreamSupervisionPolicy policy)
    : channel_(channel), policy_(policy) {}

std::chrono::milliseconds ReconnectBackoff::NextDelay(
    bool reached_ready_state,
    std::chrono::milliseconds session_lifetime) {
    if (reached_ready_state &&
        session_lifetime >= policy_.stable_session)
        failure_count_ = 0;

    const std::size_t exponent =
        std::min<std::size_t>(failure_count_, 16);
    const std::uint64_t multiplier = std::uint64_t{1} << exponent;
    const std::uint64_t initial =
        static_cast<std::uint64_t>(
            std::max<std::int64_t>(
                0, policy_.initial_retry.count()));
    const std::uint64_t maximum =
        static_cast<std::uint64_t>(
            std::max<std::int64_t>(
                0, policy_.maximum_retry.count()));
    const std::uint64_t uncapped =
        initial > 0 &&
                multiplier >
                    std::numeric_limits<std::uint64_t>::max() / initial
            ? maximum
            : initial * multiplier;
    const std::uint64_t base = std::min(uncapped, maximum);

    const std::uint64_t seed =
        Mix(static_cast<std::uint64_t>(failure_count_ + 1) ^
            (channel_ == StreamChannel::MarketData
                 ? 0x4d41524b4554ULL
                 : 0x4143434f554e54ULL));
    const std::int64_t jitter_span =
        static_cast<std::int64_t>(base / 5);
    const std::int64_t jitter =
        jitter_span == 0
            ? 0
            : static_cast<std::int64_t>(
                  seed % static_cast<std::uint64_t>(
                             jitter_span * 2 + 1)) -
                  jitter_span;
    ++failure_count_;
    return std::chrono::milliseconds{
        std::clamp<std::int64_t>(
            static_cast<std::int64_t>(base) + jitter,
            0, static_cast<std::int64_t>(maximum))};
}

void ReconnectBackoff::Reset() {
    failure_count_ = 0;
}

SubscriptionRecovery EvaluateSubscription(
    const std::vector<std::string>& desired,
    const std::vector<std::string>& acknowledged_trades,
    const std::vector<std::string>& acknowledged_quotes,
    const std::vector<std::string>& acknowledged_statuses,
    std::size_t consecutive_mismatches) {
    if (Normalize(desired) == Normalize(acknowledged_trades) &&
        Normalize(desired) == Normalize(acknowledged_quotes) &&
        Normalize(acknowledged_statuses) ==
            std::vector<std::string>{"*"})
        return SubscriptionRecovery::Ready;
    return consecutive_mismatches >= 3
               ? SubscriptionRecovery::Restart
               : SubscriptionRecovery::Repair;
}

bool InterruptibleReconnectWait(
    const std::atomic<bool>& running,
    std::chrono::milliseconds delay) {
    constexpr auto quantum = std::chrono::milliseconds(100);
    while (running && delay > std::chrono::milliseconds::zero()) {
        const auto step = std::min(delay, quantum);
        std::this_thread::sleep_for(step);
        delay -= step;
    }
    return running;
}

bool HasRecoverableMinuteGap(
    std::int64_t disconnected_at_ns,
    std::int64_t reconnected_at_ns) {
    if (disconnected_at_ns <= 0 ||
        reconnected_at_ns <= disconnected_at_ns)
        return false;
    const std::int64_t first_minute =
        disconnected_at_ns / kMinuteNs * kMinuteNs;
    const std::int64_t completed_end =
        reconnected_at_ns / kMinuteNs * kMinuteNs;
    return completed_end > first_minute;
}

}  // namespace tradebox::broker::alpaca
