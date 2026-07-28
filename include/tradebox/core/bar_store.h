#pragma once

#include "tradebox/core/bar_series.h"
#include "tradebox/core/sequence_ring.h"

#include <mutex>
#include <unordered_map>

namespace tradebox::core {

class BarStore final : public IBarDataSink, public IBarDataView {
public:
    explicit BarStore(
        std::size_t maximum_changed_series = 65'536)
        : changes_(maximum_changed_series) {}

    bool Upsert(BarUpsertBatch batch) override;
    BarSeriesSnapshot Bars(
        const BarSeriesKey& key, BarRange range) const override;
    ChangedBarSeriesBatch BarChanges(
        std::uint64_t after_sequence,
        std::size_t maximum_series) const override;

private:
    struct KeyHash {
        std::size_t operator()(const BarSeriesKey& key) const;
    };

    struct SeriesState {
        std::string symbol;
        std::vector<MarketBar> bars;
        std::vector<BarRange> coverage;
        std::uint64_t revision = 0;
        std::uint64_t change_sequence = 0;
    };

    static void MergeCoverage(
        std::vector<BarRange>& coverage, BarRange added);
    static std::vector<BarRange> MissingRanges(
        const std::vector<BarRange>& coverage, BarRange requested);
    void RecordChange(const BarSeriesKey& key,
                      SeriesState& state);

    mutable std::mutex mutex_;
    std::unordered_map<BarSeriesKey, SeriesState, KeyHash> series_;
    std::uint64_t next_change_sequence_ = 1;
    SequenceRing<ChangedBarSeries> changes_;
};

}  // namespace tradebox::core
