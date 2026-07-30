#pragma once

#include "tradebox/core/bar_series.h"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tradebox::broker::alpaca {

struct HistoricalBarPageRequest {
    std::string symbol;
    core::BarSeriesKey key;
    core::BarRange range;
    std::string page_token;
    std::size_t limit = 10'000;
};

[[nodiscard]] std::string BuildHistoricalBarPath(
    const HistoricalBarPageRequest& request);

class InFlightBarRanges {
public:
    [[nodiscard]] std::vector<core::BarRange> Reserve(
        const core::BarSeriesKey& key,
        const std::vector<core::BarRange>& missing);
    void Release(const core::BarSeriesKey& key,
                 const std::vector<core::BarRange>& ranges);

private:
    struct KeyHash {
        std::size_t operator()(
            const core::BarSeriesKey& key) const;
    };

    std::mutex mutex_;
    std::unordered_map<
        core::BarSeriesKey, std::vector<core::BarRange>, KeyHash>
        active_;
};

}  // namespace tradebox::broker::alpaca
