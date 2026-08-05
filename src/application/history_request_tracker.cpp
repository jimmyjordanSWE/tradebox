#include "tradebox/application/history_request_tracker.h"

#include <algorithm>
#include <limits>

namespace tradebox::application {
namespace {

bool Overlaps(core::BarRange left, core::BarRange right) {
    return left.start_ns < right.end_ns && right.start_ns < left.end_ns;
}

}  // namespace

void HistoryRequestTracker::Publish(
    broker::HistoryRequestStatus status) {
    constexpr std::size_t maximum_retained_statuses = 512;
    std::scoped_lock lock(mutex_);
    const auto found = std::ranges::find_if(
        statuses_, [&](const auto& existing) {
            return existing.status.key == status.key &&
                   existing.status.range == status.range;
        });
    if (found == statuses_.end())
        statuses_.push_back({
            .status = std::move(status),
            .sequence = next_sequence_++,
        });
    else {
        found->status = std::move(status);
        found->sequence = next_sequence_++;
    }
    while (statuses_.size() > maximum_retained_statuses) {
        auto oldest = std::ranges::min_element(
            statuses_, {}, &Entry::sequence);
        const auto oldest_completed = std::ranges::min_element(
            statuses_, {}, [](const Entry& entry) {
                return entry.status.state ==
                               broker::HistoryRequestState::Loading
                           ? std::numeric_limits<std::uint64_t>::max()
                           : entry.sequence;
            });
        statuses_.erase(
            oldest_completed != statuses_.end() &&
                    oldest_completed->status.state !=
                        broker::HistoryRequestState::Loading
                ? oldest_completed
                : oldest);
    }
}

std::optional<broker::HistoryRequestStatus>
HistoryRequestTracker::StatusFor(
    const core::BarSeriesKey& key, core::BarRange range) const {
    std::scoped_lock lock(mutex_);
    std::optional<broker::HistoryRequestStatus> result;
    std::uint64_t result_sequence = 0;
    for (const auto& entry : statuses_) {
        if (entry.status.key != key ||
            !Overlaps(entry.status.range, range) ||
            entry.sequence <= result_sequence)
            continue;
        result = entry.status;
        result_sequence = entry.sequence;
    }
    return result;
}

}  // namespace tradebox::application
