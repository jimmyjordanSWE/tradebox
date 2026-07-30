#include "tradebox/core/bar_series.h"

#include <algorithm>
#include <charconv>

namespace tradebox::core {
namespace {

constexpr std::int64_t kMinuteNs = 60LL * 1'000'000'000;
constexpr std::int64_t kHourNs = 60LL * kMinuteNs;

std::optional<int> TimeframeCount(
    std::string_view timeframe, std::string_view suffix) {
    if (!timeframe.ends_with(suffix) ||
        timeframe.size() <= suffix.size())
        return std::nullopt;
    const std::string_view number =
        timeframe.substr(0, timeframe.size() - suffix.size());
    int count = 0;
    const auto parsed = std::from_chars(
        number.data(), number.data() + number.size(), count);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != number.data() + number.size())
        return std::nullopt;
    return count;
}

}  // namespace

void MergeBarRange(
    std::vector<BarRange>& ranges, BarRange added) {
    if (added.start_ns >= added.end_ns) return;
    std::vector<BarRange> merged;
    merged.reserve(ranges.size() + 1);
    bool inserted = false;
    for (const BarRange range : ranges) {
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
    ranges = std::move(merged);
}

std::vector<BarRange> MissingBarRanges(
    const std::vector<BarRange>& covered,
    BarRange requested) {
    std::vector<BarRange> missing;
    if (requested.start_ns >= requested.end_ns) return missing;
    std::int64_t cursor = requested.start_ns;
    for (const BarRange range : covered) {
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

std::vector<BarRange> SubtractBarRanges(
    const std::vector<BarRange>& requested,
    const std::vector<BarRange>& excluded) {
    std::vector<BarRange> result;
    for (const BarRange range : requested) {
        const auto remaining = MissingBarRanges(excluded, range);
        result.insert(result.end(), remaining.begin(), remaining.end());
    }
    return result;
}

std::optional<std::int64_t> FixedBarDurationNs(
    std::string_view timeframe) {
    if (const auto count =
            TimeframeCount(timeframe, "Min");
        count && *count >= 1 && *count <= 59)
        return *count * kMinuteNs;
    if (const auto count = TimeframeCount(timeframe, "T");
        count && *count >= 1 && *count <= 59)
        return *count * kMinuteNs;
    if (const auto count =
            TimeframeCount(timeframe, "Hour");
        count && *count >= 1 && *count <= 23)
        return *count * kHourNs;
    if (const auto count = TimeframeCount(timeframe, "H");
        count && *count >= 1 && *count <= 23)
        return *count * kHourNs;
    return std::nullopt;
}

void ConvergeLiveBar(
    BarSeriesSnapshot& snapshot,
    const MarketDataSnapshot& live,
    const std::vector<MarketBar>& finalized_raw_minutes) {
    snapshot.current_bar.reset();
    snapshot.latest_price.reset();

    // Provider-owned open bars, including the live daily bar, share
    // the same generic current-bar contract.
    for (auto position = snapshot.bars.begin();
         position != snapshot.bars.end();) {
        if (position->state != BarState::Open) {
            ++position;
            continue;
        }
        if (!snapshot.current_bar ||
            position->start_ns >
                snapshot.current_bar->start_ns ||
            (position->start_ns ==
                 snapshot.current_bar->start_ns &&
             position->revision >
                 snapshot.current_bar->revision))
            snapshot.current_bar = *position;
        position = snapshot.bars.erase(position);
    }

    if (snapshot.key.adjustment != BarAdjustment::Raw ||
        snapshot.key.feed == MarketDataFeed::Unknown ||
        live.feed != snapshot.key.feed ||
        live.stream_status != MarketStreamStatus::Subscribed ||
        !live.trades_subscribed ||
        (!snapshot.key.instrument_id.empty() &&
         !live.instrument_id.empty() &&
         snapshot.key.instrument_id != live.instrument_id))
        return;

    snapshot.latest_price = live.latest_price;
    const auto duration =
        FixedBarDurationNs(snapshot.key.timeframe);
    if (!duration) return;

    const ProvisionalMinuteBar* newest = nullptr;
    for (const ProvisionalMinuteBar& provisional :
         live.provisional_minute_bars) {
        if (!newest ||
            provisional.start_ns > newest->start_ns ||
            (provisional.start_ns == newest->start_ns &&
             provisional.revision > newest->revision))
            newest = &provisional;
    }
    if (!newest) return;

    const std::int64_t target_start =
        (newest->start_ns / *duration) * *duration;
    if (target_start < snapshot.requested_range.start_ns ||
        target_start >= snapshot.requested_range.end_ns)
        return;

    const bool matching_provider_bar = std::ranges::any_of(
        snapshot.bars, [&](const MarketBar& bar) {
            return bar.start_ns == target_start;
        });
    if (*duration == kMinuteNs && matching_provider_bar) {
        snapshot.current_bar.reset();
        return;
    }

    // Never claim a complete larger candle when observation began
    // partway through its interval. A provider open bar may still be
    // returned above until the next fully observed interval begins.
    if (live.projection_started_at_ns > target_start)
        return;

    MarketBar provisional_minute{
        .start_ns = newest->start_ns,
        .open = newest->open,
        .high = newest->high,
        .low = newest->low,
        .close = newest->close,
        .volume = newest->volume,
        .trade_count = newest->trade_count,
        .source = BarSource::DerivedTicks,
        .state = BarState::Open,
        .revision = newest->revision,
    };
    std::vector<const MarketBar*> components;
    components.reserve(finalized_raw_minutes.size() + 1);
    bool provider_has_current_minute = false;
    for (const MarketBar& minute : finalized_raw_minutes) {
        if (minute.start_ns < target_start ||
            minute.start_ns > newest->start_ns)
            continue;
        components.push_back(&minute);
        provider_has_current_minute =
            provider_has_current_minute ||
            minute.start_ns == newest->start_ns;
    }
    if (!provider_has_current_minute)
        components.push_back(&provisional_minute);
    if (components.empty()) return;
    std::ranges::sort(
        components,
        [](const MarketBar* left, const MarketBar* right) {
            return left->start_ns < right->start_ns;
        });

    MarketBar current{
        .start_ns = target_start,
        .open = components.front()->open,
        .high = components.front()->high,
        .low = components.front()->low,
        .close = components.front()->close,
        .source = BarSource::DerivedTicks,
        .state = BarState::Open,
    };
    for (const MarketBar* component : components) {
        current.high = std::max(current.high, component->high);
        current.low = std::min(current.low, component->low);
        current.close = component->close;
        current.volume += component->volume;
        current.trade_count += component->trade_count;
        current.revision =
            std::max(current.revision, component->revision);
    }

    std::erase_if(snapshot.bars, [&](const MarketBar& bar) {
        return bar.start_ns == target_start;
    });
    snapshot.current_bar = std::move(current);
}

}  // namespace tradebox::core
