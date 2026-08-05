#pragma once

#include "tradebox/broker/history_status.h"

#include <mutex>
#include <optional>
#include <cstdint>
#include <vector>

namespace tradebox::application {

class HistoryRequestTracker final
    : public broker::IHistoryRequestStatusSink {
public:
    void Publish(broker::HistoryRequestStatus status) override;
    [[nodiscard]] std::optional<broker::HistoryRequestStatus> StatusFor(
        const core::BarSeriesKey& key, core::BarRange range) const;

private:
    struct Entry {
        broker::HistoryRequestStatus status;
        std::uint64_t sequence = 0;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> statuses_;
    std::uint64_t next_sequence_ = 1;
};

}  // namespace tradebox::application
