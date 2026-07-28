#pragma once

#include "tradebox/core/market_data.h"
#include "tradebox/core/bar_series.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tradebox::broker::alpaca {

enum class StreamControlType {
    Authenticated,
    Error,
    Subscription,
};

struct StreamControl {
    StreamControlType type = StreamControlType::Error;
    std::string message;
    std::vector<std::string> trade_symbols;
    std::vector<std::string> quote_symbols;
};

struct DecodedBar {
    std::string kind;
    std::string instrument_id;
    std::string symbol;
    std::int64_t timestamp_ms = 0;
    tradebox::core::Decimal open;
    tradebox::core::Decimal high;
    tradebox::core::Decimal low;
    tradebox::core::Decimal close;
    tradebox::core::Decimal volume;
    std::optional<tradebox::core::Decimal> within_bar_vwap;
    std::uint64_t trade_count = 0;
};

struct DecodedStreamItem {
    bool market_tick = false;
    std::string source_event_id;
    std::string kind;
    std::string symbol;
    std::int64_t event_time_ns = 0;
    std::int64_t received_at_ms = 0;
    tradebox::core::MarketDataEventPtr market_event;
    std::optional<DecodedBar> bar;
};

struct DecodedMarketFrame {
    std::vector<StreamControl> controls;
    std::vector<DecodedStreamItem> items;
    std::size_t ignored_items = 0;
};

using InstrumentResolver =
    std::function<std::string(std::string_view symbol)>;

DecodedMarketFrame DecodeMarketFrame(
    std::string_view raw_frame, std::int64_t received_at_ms,
    const InstrumentResolver& resolve_instrument);

}  // namespace tradebox::broker::alpaca
