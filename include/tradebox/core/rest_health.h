#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tradebox::core {

struct RestRateBudget {
    std::int64_t limit = -1;
    std::int64_t remaining = -1;
    std::int64_t reset_at_ms = 0;
};

struct RestTransportHealth {
    RestRateBudget trading;
    RestRateBudget market_data;
    std::size_t queued = 0;
    std::size_t in_flight = 0;
    std::size_t queue_high_water = 0;
    std::uint64_t completed = 0;
    std::uint64_t retries = 0;
    std::uint64_t coalesced = 0;
    std::uint64_t rejected = 0;
    std::uint64_t http_rejected = 0;
    std::uint64_t canceled = 0;
    std::size_t background_queued = 0;
    std::size_t background_in_flight = 0;
    std::uint64_t background_rejected = 0;
    std::uint64_t background_coalesced = 0;
    bool stopping = false;
};

struct MarketDataPipelineHealth {
    std::uint64_t candlestick_bytes = 0;
    std::uint64_t tick_bytes = 0;
    std::uint64_t database_bytes = 0;
    std::uint64_t candlestick_rows = 0;
    std::uint64_t tick_rows = 0;
    std::uint64_t pending_events = 0;
    std::uint64_t high_water_events = 0;
    std::uint64_t dropped_market_events = 0;
    std::uint64_t pending_bars = 0;
    std::uint64_t high_water_bars = 0;
    std::uint64_t dropped_bars = 0;
    std::uint64_t persistence_failures = 0;
    std::string last_persistence_error;
    // V1.0 intentionally retains all captured market data.
    bool retention_limited = false;
    bool overloaded = false;
};

}  // namespace tradebox::core
