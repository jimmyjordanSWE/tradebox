#pragma once

#include "tradebox/core/order_command.h"
#include "tradebox/core/asset_catalog.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct Bar {
    std::int64_t timestamp_ms = 0;
    double open = 0;
    double high = 0;
    double low = 0;
    double close = 0;
    double volume = 0;
};

struct MarketClockSnapshot {
    bool is_open = false;
    std::int64_t timestamp_ms = 0;
    std::int64_t next_open_ms = 0;
    std::int64_t next_close_ms = 0;
    std::int64_t received_at_ms = 0;
};

enum class UiEventType {
    Status,
    MarketClock,
    HistoricalBars,
    Trade,
    DailyBar,
    AccountStreamConnected,
    AccountStreamEvent,
    OrderCommandCompleted,
    AssetCatalogReady
};

enum class OperationalComponent {
    None,
    Account,
    AccountStream,
    MarketDataStream,
    Persistence,
};

enum class OperationalState {
    None,
    Connecting,
    Reconnecting,
    Authenticated,
    Subscribed,
    Degraded,
    Disconnected,
    Failed,
};

enum class OperationalReason {
    None,
    TransportFailure,
    UpgradeFailure,
    AuthenticationFailure,
    SubscriptionMismatch,
    UnexpectedDisconnect,
    SilenceTimeout,
    QueueOverload,
    PersistenceFailure,
    SecurityPolicyFailure,
    PayloadLimitExceeded,
};

enum class OperationalSeverity {
    Informational,
    Warning,
    Critical,
};

struct UiEvent {
    UiEventType type = UiEventType::Status;
    std::string symbol;
    std::string message;
    std::vector<Bar> bars;
    Bar bar;
    MarketClockSnapshot market_clock;
    std::int64_t received_at_ms = 0;
    std::int64_t latency_ms = -1;
    std::string request_id;
    tradebox::core::OrderCommandResult command_result;
    std::string timeframe = "1Day";
    std::vector<tradebox::core::TradableAsset> assets;
    OperationalComponent operational_component =
        OperationalComponent::None;
    OperationalState operational_state = OperationalState::None;
    OperationalReason operational_reason = OperationalReason::None;
    OperationalSeverity operational_severity =
        OperationalSeverity::Informational;
    std::uint32_t retry_attempt = 0;
    std::int64_t retry_in_ms = 0;
};

class UiEventQueue {
public:
    void Push(UiEvent event) {
        std::scoped_lock lock(mutex_);
        queue_.push_back(std::move(event));
    }

    std::vector<UiEvent> Drain() {
        std::vector<UiEvent> result;
        std::scoped_lock lock(mutex_);
        result.reserve(queue_.size());
        while (!queue_.empty()) {
            result.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return result;
    }

private:
    std::mutex mutex_;
    std::deque<UiEvent> queue_;
};
