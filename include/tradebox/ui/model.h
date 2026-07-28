#pragma once

#include "tradebox/core/order_command.h"
#include "tradebox/core/order_request.h"
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

struct OrderEntryDraft {
    std::string name = "Untitled order";
    std::string symbol = "AMD";
    std::string side = "Buy";
    std::string amount = "1";
    bool amount_is_notional = false;
    std::string type = "Market";
    std::string limit_price;
    std::string stop_price;
    std::string time_in_force = "Day";
    bool extended_hours = false;
};

struct UiValidationMessage {
    std::string field;
    std::string message;
};

enum class UiOrderState {
    Pending, Accepted, Rejected, Canceled, Filled, Indeterminate, Stale,
    Reconciling
};

[[nodiscard]] std::vector<UiValidationMessage> ValidateOrderEntry(
    const OrderEntryDraft& draft);
[[nodiscard]] std::string UiOrderStateLabel(UiOrderState state);
[[nodiscard]] UiOrderState UiOrderStateFromCore(
    const tradebox::core::OrderState& order,
    const tradebox::core::CoreSnapshot& snapshot,
    bool command_indeterminate = false);

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
