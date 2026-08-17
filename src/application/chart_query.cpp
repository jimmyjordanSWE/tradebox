#include "tradebox/application/chart_query.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace tradebox::application {
namespace {

std::optional<std::int64_t> ChartStepNs(
    const std::string& timeframe) {
    if (const auto fixed = core::FixedBarDurationNs(timeframe))
        return fixed;
    if (timeframe == "1Day" || timeframe == "1D")
        return 24LL * 60 * 60 * 1'000'000'000;
    if (timeframe == "1Week" || timeframe == "1W")
        return 7LL * 24 * 60 * 60 * 1'000'000'000;
    if (timeframe == "1Month" || timeframe == "1M")
        return 31LL * 24 * 60 * 60 * 1'000'000'000;
    return std::nullopt;
}

std::optional<std::int64_t> CheckedProduct(
    std::size_t count, std::int64_t step) {
    if (step <= 0 || count > static_cast<std::size_t>(
                                std::numeric_limits<std::int64_t>::max() /
                                step))
        return std::nullopt;
    return static_cast<std::int64_t>(count) * step;
}

}  // namespace

std::expected<core::BarRange, std::string> ResolveChartRange(
    const ChartViewportIntent& intent,
    const ChartRangePolicy& policy) {
    if (intent.document_id.empty())
        return std::unexpected("Chart document identity is required");
    if (intent.key.instrument_id.empty() || intent.symbol.empty())
        return std::unexpected("Resolved instrument identity is required");
    if (intent.anchor_ns <= 0)
        return std::unexpected("Chart range anchor must be positive");
    if (policy.minimum_visible_bars == 0 ||
        policy.minimum_visible_bars > policy.maximum_visible_bars ||
        policy.history_multiplier == 0)
        return std::unexpected("Chart range policy is invalid");
    const auto step = ChartStepNs(intent.key.timeframe);
    if (!step)
        return std::unexpected("Unsupported chart timeframe");
    // A follow-latest chart supplies the current wall-clock timestamp on every
    // frame. Resolve that moving timestamp to bar resolution so an unchanged
    // viewport continues to describe one stable historical-data demand.
    const std::int64_t anchor_ns =
        intent.anchor_ns - (intent.anchor_ns % *step);
    const std::size_t visible = std::clamp(
        intent.visible_bars, policy.minimum_visible_bars,
        policy.maximum_visible_bars);
    if (visible > std::numeric_limits<std::size_t>::max() /
                      policy.history_multiplier)
        return std::unexpected("Chart history range is too large");
    const auto history = CheckedProduct(
        visible * policy.history_multiplier, *step);
    const auto future = CheckedProduct(policy.future_bars, *step);
    if (!history || !future || anchor_ns < *history ||
        anchor_ns >
            std::numeric_limits<std::int64_t>::max() - *future)
        return std::unexpected("Chart range exceeds timestamp limits");
    return core::BarRange{
        .start_ns = anchor_ns - *history,
        .end_ns = anchor_ns + *future,
    };
}

}  // namespace tradebox::application
