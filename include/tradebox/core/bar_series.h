#pragma once

#include "tradebox/core/decimal.h"
#include "tradebox/core/market_data.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tradebox::core {

enum class BarAdjustment {
    Raw,
    Split,
    Dividend,
    All,
};

enum class BarSource {
    ProviderHistorical,
    ProviderStream,
    DerivedTicks,
};

enum class BarState {
    Open,
    Finalized,
    Revised,
};

struct BarSeriesKey {
    std::string instrument_id;
    MarketDataFeed feed = MarketDataFeed::Unknown;
    std::string timeframe = "1Min";
    BarAdjustment adjustment = BarAdjustment::Raw;

    bool operator==(const BarSeriesKey&) const = default;
};

// All ranges are half-open: [start_ns, end_ns).
struct BarRange {
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;

    bool operator==(const BarRange&) const = default;
};

struct MarketBar {
    std::int64_t start_ns = 0;
    Decimal open;
    Decimal high;
    Decimal low;
    Decimal close;
    Decimal volume;
    // VWAP scoped to this bar interval and qualified by key.feed.
    // This is source data, not a session/anchored VWAP study.
    std::optional<Decimal> within_bar_vwap;
    std::uint64_t trade_count = 0;
    BarSource source = BarSource::ProviderHistorical;
    BarState state = BarState::Finalized;
    std::uint64_t revision = 0;
};

struct BarUpsertBatch {
    BarSeriesKey key;
    std::string symbol;
    std::vector<MarketBar> bars;
    std::optional<BarRange> covered_range;
};

struct BarSeriesSnapshot {
    BarSeriesKey key;
    std::string symbol;
    BarRange requested_range;
    std::vector<MarketBar> bars;
    std::vector<BarRange> missing_ranges;
    std::uint64_t revision = 0;
};

struct ChangedBarSeries {
    std::uint64_t sequence = 0;
    BarSeriesKey key;
    std::string symbol;
};

struct ChangedBarSeriesBatch {
    std::vector<ChangedBarSeries> series;
    std::uint64_t next_sequence = 0;
    bool gap_detected = false;
};

class IBarDataSink {
public:
    virtual ~IBarDataSink() = default;
    virtual bool Upsert(BarUpsertBatch batch) = 0;
};

class IBarDataView {
public:
    virtual ~IBarDataView() = default;
    virtual BarSeriesSnapshot Bars(
        const BarSeriesKey& key, BarRange range) const = 0;
    virtual ChangedBarSeriesBatch BarChanges(
        std::uint64_t after_sequence,
        std::size_t maximum_series) const = 0;
};

}  // namespace tradebox::core
