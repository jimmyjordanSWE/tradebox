#pragma once

#include "tradebox/core/indicator.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace tradebox::application {

struct IndicatorProjectionSnapshot {
    std::uint64_t source_revision = 0;
    std::vector<core::IndicatorSeries> series;
    std::vector<std::string> errors;
};

// Retains bounded immutable indicator publications keyed by their exact input
// series revision and typed graph. Identical consumers share one evaluation.
class IndicatorProjectionCache final {
public:
    explicit IndicatorProjectionCache(
        std::size_t maximum_entries = 512);

    [[nodiscard]] std::shared_ptr<const IndicatorProjectionSnapshot> Resolve(
        const core::BarSeriesSnapshot& series,
        std::span<const core::IndicatorDefinition> definitions);

private:
    struct Entry {
        std::shared_ptr<const IndicatorProjectionSnapshot> snapshot;
        std::uint64_t access_sequence = 0;
    };

    [[nodiscard]] static std::string Key(
        const core::BarSeriesSnapshot& series,
        std::span<const core::IndicatorDefinition> definitions);
    void EvictIfNeeded();

    const std::size_t maximum_entries_;
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::uint64_t next_access_sequence_ = 1;
};

}  // namespace tradebox::application
