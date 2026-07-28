#include "tradebox/core/bar_store.h"

#include <algorithm>
#include <functional>

namespace tradebox::core {
namespace {

void HashCombine(std::size_t& seed, std::size_t value) {
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

bool SamePayload(const MarketBar& left, const MarketBar& right) {
    return left.open == right.open &&
           left.high == right.high &&
           left.low == right.low &&
           left.close == right.close &&
           left.volume == right.volume &&
           left.within_bar_vwap ==
               right.within_bar_vwap &&
           left.trade_count == right.trade_count &&
           left.source == right.source &&
           left.state == right.state;
}

}  // namespace

std::size_t BarStore::KeyHash::operator()(
    const BarSeriesKey& key) const {
    std::size_t result =
        std::hash<std::string>{}(key.instrument_id);
    HashCombine(
        result,
        std::hash<int>{}(static_cast<int>(key.feed)));
    HashCombine(result, std::hash<std::string>{}(key.timeframe));
    HashCombine(
        result,
        std::hash<int>{}(static_cast<int>(key.adjustment)));
    return result;
}

bool BarStore::Upsert(BarUpsertBatch batch) {
    if (batch.key.instrument_id.empty()) return false;
    std::scoped_lock lock(mutex_);
    SeriesState& state = series_[batch.key];
    bool changed = false;
    if (!batch.symbol.empty() && state.symbol != batch.symbol) {
        state.symbol = std::move(batch.symbol);
        changed = true;
    }

    std::ranges::sort(batch.bars, {}, &MarketBar::start_ns);
    for (MarketBar& incoming : batch.bars) {
        const auto position = std::ranges::lower_bound(
            state.bars, incoming.start_ns, {},
            &MarketBar::start_ns);
        if (position == state.bars.end() ||
            position->start_ns != incoming.start_ns) {
            incoming.revision =
                std::max<std::uint64_t>(incoming.revision, 1);
            state.bars.insert(position, std::move(incoming));
            changed = true;
            continue;
        }
        if (SamePayload(*position, incoming)) continue;
        incoming.revision = position->revision + 1;
        if (position->state == BarState::Finalized ||
            position->state == BarState::Revised)
            incoming.state = BarState::Revised;
        *position = std::move(incoming);
        changed = true;
    }

    if (batch.covered_range &&
        batch.covered_range->start_ns <
            batch.covered_range->end_ns) {
        const std::vector<BarRange> previous = state.coverage;
        MergeCoverage(state.coverage, *batch.covered_range);
        changed = changed || previous != state.coverage;
    }
    if (changed) {
        ++state.revision;
        RecordChange(batch.key, state);
    }
    return changed;
}

BarSeriesSnapshot BarStore::Bars(
    const BarSeriesKey& key, BarRange range) const {
    std::scoped_lock lock(mutex_);
    BarSeriesSnapshot result{
        .key = key,
        .requested_range = range,
    };
    const auto found = series_.find(key);
    if (found == series_.end()) {
        if (range.start_ns < range.end_ns)
            result.missing_ranges.push_back(range);
        return result;
    }
    const SeriesState& state = found->second;
    result.symbol = state.symbol;
    result.revision = state.revision;
    const auto first = std::ranges::lower_bound(
        state.bars, range.start_ns, {}, &MarketBar::start_ns);
    const auto last = std::ranges::lower_bound(
        state.bars, range.end_ns, {}, &MarketBar::start_ns);
    result.bars.assign(first, last);
    result.missing_ranges =
        MissingRanges(state.coverage, range);
    return result;
}

ChangedBarSeriesBatch BarStore::BarChanges(
    std::uint64_t after_sequence,
    std::size_t maximum_series) const {
    std::scoped_lock lock(mutex_);
    ChangedBarSeriesBatch result{
        .next_sequence = after_sequence,
    };
    if (maximum_series == 0 || changes_.Empty()) return result;
    result.gap_detected =
        after_sequence != 0 &&
        after_sequence + 1 < changes_.Oldest();
    std::unordered_map<BarSeriesKey, ChangedBarSeries, KeyHash>
        latest;
    latest.reserve(
        std::min(maximum_series, changes_.Size()));
    changes_.VisitAfter(
        after_sequence, changes_.Size(),
        [&](const auto& entry) {
            ChangedBarSeries changed = entry.value;
            changed.sequence = entry.sequence;
            latest.insert_or_assign(changed.key,
                                    std::move(changed));
        });
    result.series.reserve(
        std::min(maximum_series, latest.size()));
    for (auto& [key, changed] : latest) {
        static_cast<void>(key);
        result.series.push_back(std::move(changed));
    }
    std::ranges::sort(result.series, {},
                      &ChangedBarSeries::sequence);
    if (result.series.size() > maximum_series)
        result.series.resize(maximum_series);
    if (!result.series.empty())
        result.next_sequence = result.series.back().sequence;
    else
        result.next_sequence =
            std::max(after_sequence, changes_.Newest());
    return result;
}

void BarStore::MergeCoverage(
    std::vector<BarRange>& coverage, BarRange added) {
    std::vector<BarRange> merged;
    merged.reserve(coverage.size() + 1);
    bool inserted = false;
    for (const BarRange range : coverage) {
        if (range.end_ns < added.start_ns) {
            merged.push_back(range);
        } else if (added.end_ns < range.start_ns) {
            if (!inserted) {
                merged.push_back(added);
                inserted = true;
            }
            merged.push_back(range);
        } else {
            added.start_ns =
                std::min(added.start_ns, range.start_ns);
            added.end_ns =
                std::max(added.end_ns, range.end_ns);
        }
    }
    if (!inserted) merged.push_back(added);
    coverage = std::move(merged);
}

std::vector<BarRange> BarStore::MissingRanges(
    const std::vector<BarRange>& coverage, BarRange requested) {
    std::vector<BarRange> missing;
    if (requested.start_ns >= requested.end_ns) return missing;
    std::int64_t cursor = requested.start_ns;
    for (const BarRange range : coverage) {
        if (range.end_ns <= cursor) continue;
        if (range.start_ns >= requested.end_ns) break;
        if (range.start_ns > cursor)
            missing.push_back({
                cursor,
                std::min(range.start_ns, requested.end_ns),
            });
        cursor = std::max(cursor, range.end_ns);
        if (cursor >= requested.end_ns) break;
    }
    if (cursor < requested.end_ns)
        missing.push_back({cursor, requested.end_ns});
    return missing;
}

void BarStore::RecordChange(
    const BarSeriesKey& key, SeriesState& state) {
    state.change_sequence = next_change_sequence_++;
    changes_.Push(
        state.change_sequence,
        ChangedBarSeries{
            .sequence = state.change_sequence,
            .key = key,
            .symbol = state.symbol,
        });
}

}  // namespace tradebox::core
