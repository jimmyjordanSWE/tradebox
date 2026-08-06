#include "tradebox/application/indicator_projection_cache.h"

#include <algorithm>
#include <sstream>
#include <type_traits>

namespace tradebox::application {

IndicatorProjectionCache::IndicatorProjectionCache(
    std::size_t maximum_entries)
    : maximum_entries_(std::max<std::size_t>(maximum_entries, 1)) {}

std::string IndicatorProjectionCache::Key(
    const core::BarSeriesSnapshot& series,
    std::span<const core::IndicatorDefinition> definitions) {
    std::ostringstream output;
    const auto append_string = [&output](std::string_view value) {
        output << value.size() << ':' << value << ';';
    };
    append_string(series.key.instrument_id);
    output << static_cast<int>(series.key.feed) << ';';
    append_string(series.key.timeframe);
    output << static_cast<int>(series.key.adjustment) << ';'
           << series.requested_range.start_ns << ';'
           << series.requested_range.end_ns << ';'
           << series.revision << ';'
           << (series.current_bar ? series.current_bar->start_ns : 0) << ';'
           << (series.current_bar ? series.current_bar->revision : 0) << ';'
           << (series.latest_price
                   ? series.latest_price->receive_sequence
                   : 0) << ';';
    for (const core::IndicatorDefinition& definition : definitions) {
        append_string(definition.id);
        std::visit(
            [&](const auto& calculation) {
                using T = std::decay_t<decltype(calculation)>;
                output << (std::is_same_v<
                                   T, core::SimpleMovingAverageCalculation>
                               ? "sma;"
                               : "ema;")
                       << calculation.period << ';';
                std::visit(
                    [&](const auto& input) {
                        using Input = std::decay_t<decltype(input)>;
                        if constexpr (std::is_same_v<
                                          Input, core::BarSeriesInput>) {
                            output << "bar;"
                                   << static_cast<int>(input.field) << ';';
                        } else {
                            output << "indicator;";
                            append_string(input.indicator_id);
                            append_string(input.output_id);
                        }
                    },
                    calculation.input);
            },
            definition.calculation);
    }
    return output.str();
}

std::shared_ptr<const IndicatorProjectionSnapshot>
IndicatorProjectionCache::Resolve(
    const core::BarSeriesSnapshot& series,
    std::span<const core::IndicatorDefinition> definitions) {
    const std::string key = Key(series, definitions);
    std::scoped_lock lock(mutex_);
    if (const auto found = entries_.find(key); found != entries_.end()) {
        found->second.access_sequence = next_access_sequence_++;
        return found->second.snapshot;
    }

    std::vector<core::MarketBar> indicator_bars = series.bars;
    if (series.current_bar) {
        const auto position = std::ranges::lower_bound(
            indicator_bars, series.current_bar->start_ns,
            {}, &core::MarketBar::start_ns);
        if (position != indicator_bars.end() &&
            position->start_ns == series.current_bar->start_ns)
            *position = *series.current_bar;
        else
            indicator_bars.insert(position, *series.current_bar);
    }

    auto snapshot = std::make_shared<IndicatorProjectionSnapshot>();
    snapshot->source_revision = series.revision;
    auto evaluated = core::EvaluateIndicators(definitions, indicator_bars);
    if (evaluated)
        snapshot->series = std::move(*evaluated);
    else
        snapshot->errors.push_back(evaluated.error().message);
    entries_.emplace(
        key, Entry{snapshot, next_access_sequence_++});
    EvictIfNeeded();
    return snapshot;
}

void IndicatorProjectionCache::EvictIfNeeded() {
    while (entries_.size() > maximum_entries_) {
        const auto oldest = std::ranges::min_element(
            entries_, {}, [](const auto& entry) {
                return entry.second.access_sequence;
            });
        entries_.erase(oldest);
    }
}

}  // namespace tradebox::application
