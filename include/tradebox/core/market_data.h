#pragma once

#include "tradebox/core/decimal.h"
#include "tradebox/core/trade_pressure_types.h"
#include "tradebox/core/types.h"

#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tradebox::core {

enum class MarketStreamStatus {
    Disconnected,
    Connecting,
    Authenticated,
    Subscribed,
    Stale,
    Error,
};

struct MarketQuote {
    std::string instrument_id;
    std::string symbol;
    Decimal bid_price;
    Decimal bid_size;
    std::string bid_exchange;
    Decimal ask_price;
    Decimal ask_size;
    std::string ask_exchange;
    std::vector<std::string> conditions;
    std::string tape;
    std::string broker_timestamp;
    std::int64_t event_time_ns = 0;
    std::int64_t received_at_ms = 0;
};

struct MarketTrade {
    std::string instrument_id;
    std::string symbol;
    std::string trade_id;
    Decimal price;
    Decimal size;
    std::string exchange;
    std::vector<std::string> conditions;
    std::string tape;
    std::string broker_timestamp;
    std::int64_t event_time_ns = 0;
    std::int64_t received_at_ms = 0;
    std::uint64_t receive_sequence = 0;
    bool corrected = false;
};

struct CanonicalMarketPrice {
    Decimal price;
    std::string trade_id;
    std::string broker_timestamp;
    std::int64_t event_time_ns = 0;
    std::int64_t received_at_ms = 0;
    std::uint64_t receive_sequence = 0;
};

// A raw, feed-qualified projection of an unfinalized provider minute.
// It is intentionally separate from provider bars.
struct ProvisionalMinuteBar {
    std::int64_t start_ns = 0;
    Decimal open;
    Decimal high;
    Decimal low;
    Decimal close;
    Decimal volume;
    std::uint64_t trade_count = 0;
    std::uint64_t revision = 0;
};

struct QuoteReceived {
    MarketQuote quote;
};

struct TradeReceived {
    MarketTrade trade;
};

struct TradeCanceled {
    std::string instrument_id;
    std::string symbol;
    std::string trade_id;
    std::string broker_timestamp;
    std::int64_t event_time_ns = 0;
    std::int64_t received_at_ms = 0;
};

struct TradeCorrected {
    std::string instrument_id;
    std::string symbol;
    std::string original_trade_id;
    MarketTrade corrected_trade;
};

enum class SecurityTradingState {
    Unknown,
    Trading,
    Halted,
    Paused,
};

struct StockTradingStatus {
    std::string instrument_id;
    std::string symbol;
    SecurityTradingState state =
        SecurityTradingState::Unknown;
    std::string status_code;
    std::string status_message;
    std::string reason_code;
    std::string reason_message;
    std::string tape;
    std::string broker_timestamp;
    std::int64_t event_time_ns = 0;
    std::int64_t received_at_ms = 0;

    [[nodiscard]] bool BlocksNewOrders() const {
        return state == SecurityTradingState::Halted ||
               state == SecurityTradingState::Paused;
    }
};

struct TradingStatusReceived {
    StockTradingStatus status;
};

struct MarketStreamChanged {
    MarketStreamStatus status = MarketStreamStatus::Disconnected;
    MarketDataFeed feed = MarketDataFeed::Unknown;
    std::vector<std::string> trade_symbols;
    std::vector<std::string> quote_symbols;
    std::vector<std::string> status_symbols;
    std::string message;
    std::int64_t received_at_ms = 0;
};

using MarketDataEvent =
    std::variant<QuoteReceived, TradeReceived, TradeCanceled,
                 TradeCorrected, TradingStatusReceived,
                 MarketStreamChanged>;
using MarketDataEventPtr = std::shared_ptr<const MarketDataEvent>;

template <class Event>
MarketDataEventPtr ShareMarketDataEvent(Event event) {
    return std::make_shared<const MarketDataEvent>(
        MarketDataEvent{std::move(event)});
}

struct MarketDataSnapshot {
    std::string instrument_id;
    std::string symbol;
    MarketDataFeed feed = MarketDataFeed::Unknown;
    MarketStreamStatus stream_status =
        MarketStreamStatus::Disconnected;
    bool trades_subscribed = false;
    bool quotes_subscribed = false;
    bool statuses_subscribed = false;
    // Start of uninterrupted trade observation for this symbol/feed.
    // Zero means the boundary is unknown (primarily synthetic tests).
    std::int64_t projection_started_at_ns = 0;
    std::optional<MarketQuote> latest_quote;
    std::optional<CanonicalMarketPrice> latest_price;
    std::optional<StockTradingStatus> trading_status;
    std::vector<ProvisionalMinuteBar> provisional_minute_bars;
    std::deque<MarketTrade> trades;
    std::uint64_t revision = 0;
    std::int64_t last_received_at_ms = 0;
    std::string status_message;
    std::optional<TradePressureSnapshot> trade_pressure;
};

struct SequencedMarketDataEvent {
    std::uint64_t sequence = 0;
    MarketDataEventPtr event;
};

struct MarketDataDelta {
    std::string instrument_id;
    std::string symbol;
    MarketDataFeed feed = MarketDataFeed::Unknown;
    MarketStreamStatus stream_status =
        MarketStreamStatus::Disconnected;
    bool trades_subscribed = false;
    bool quotes_subscribed = false;
    bool statuses_subscribed = false;
    std::int64_t projection_started_at_ns = 0;
    std::optional<MarketQuote> latest_quote;
    std::optional<CanonicalMarketPrice> latest_price;
    std::optional<StockTradingStatus> trading_status;
    std::vector<ProvisionalMinuteBar> provisional_minute_bars;
    std::vector<SequencedMarketDataEvent> events;
    std::uint64_t next_sequence = 0;
    bool gap_detected = false;
    std::int64_t last_received_at_ms = 0;
    std::string status_message;
    std::optional<TradePressureSnapshot> trade_pressure;
};

struct ChangedInstrument {
    std::uint64_t sequence = 0;
    std::string instrument_id;
    std::string symbol;
};

struct ChangedInstruments {
    std::vector<ChangedInstrument> instruments;
    std::uint64_t next_sequence = 0;
    bool gap_detected = false;
};

struct TickQuery {
    std::string instrument_id;
    std::string symbol;
    std::int64_t start_ns = 0;
    std::int64_t end_ns = std::numeric_limits<std::int64_t>::max();
    MarketDataFeed feed = MarketDataFeed::Iex;
    bool include_trades = true;
    bool include_quotes = false;
};

struct TickCoverage {
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
};

struct TickSeries {
    TickQuery query;
    std::vector<MarketTrade> trades;
    std::vector<MarketQuote> quotes;
    std::vector<TickCoverage> missing_trade_ranges;
    std::vector<TickCoverage> missing_quote_ranges;
    bool complete = false;
    std::string error;
};

class IMarketDataSink {
public:
    virtual ~IMarketDataSink() = default;
    virtual void Ingest(MarketDataEventPtr event) = 0;
    virtual void IngestBatch(
        std::vector<MarketDataEventPtr> events) {
        for (MarketDataEventPtr& event : events)
            Ingest(std::move(event));
    }
    void Ingest(MarketDataEvent event) {
        Ingest(std::make_shared<const MarketDataEvent>(
            std::move(event)));
    }
};

class IMarketDataView {
public:
    virtual ~IMarketDataView() = default;
    virtual MarketDataSnapshot Snapshot(
        const std::string& symbol) const = 0;
    virtual MarketDataDelta Delta(
        const std::string& symbol, std::uint64_t after_sequence,
        std::size_t maximum_events) const = 0;
    virtual ChangedInstruments Changes(
        std::uint64_t after_sequence,
        std::size_t maximum_instruments) const = 0;
};

}  // namespace tradebox::core
