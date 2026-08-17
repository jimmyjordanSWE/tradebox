#include "chart_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tradebox::gui::chart {
namespace {

bool Finite(double value) { return std::isfinite(value); }

double Display(const core::Decimal& value) {
    return value.ToDisplayDouble();
}

void IncludePrice(double value, double& minimum, double& maximum) {
    if (!Finite(value)) return;
    minimum = std::min(minimum, value);
    maximum = std::max(maximum, value);
}

std::int64_t ClampTimestamp(long double value) {
    if (value <= 0.0L) return 0;
    const long double maximum = static_cast<long double>(
        std::numeric_limits<std::int64_t>::max());
    if (value >= maximum)
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(value);
}

}  // namespace

std::optional<ViewportState> ZoomViewport(
    ViewportState current, core::BarRange rendered_range,
    float pointer_x, Rect plot, float wheel_delta,
    int minimum_visible_bars, int maximum_visible_bars) {
    if (!plot.IsUsable() || rendered_range.end_ns <= rendered_range.start_ns ||
        current.visible_bars <= 0 || current.anchor_ns <= 0 ||
        !Finite(pointer_x) || !Finite(wheel_delta) || wheel_delta == 0.0F ||
        minimum_visible_bars <= 0 ||
        maximum_visible_bars < minimum_visible_bars)
        return std::nullopt;
    const double factor = std::pow(0.82, static_cast<double>(wheel_delta));
    if (!Finite(factor) || factor <= 0.0) return std::nullopt;
    const double scaled =
        static_cast<double>(current.visible_bars) * factor;
    const int next_visible =
        scaled <= static_cast<double>(minimum_visible_bars)
            ? minimum_visible_bars
            : scaled >= static_cast<double>(maximum_visible_bars)
                  ? maximum_visible_bars
                  : static_cast<int>(std::lround(scaled));
    if (next_visible == current.visible_bars) return std::nullopt;

    const std::int64_t pointer_time =
        XToTime(pointer_x, rendered_range, plot);
    const long double ratio =
        static_cast<long double>(next_visible) /
        static_cast<long double>(current.visible_bars);
    const long double next_anchor =
        static_cast<long double>(pointer_time) +
        (static_cast<long double>(current.anchor_ns) -
         static_cast<long double>(pointer_time)) * ratio;
    return ViewportState{.visible_bars = next_visible,
                         .anchor_ns = ClampTimestamp(next_anchor)};
}

std::optional<std::int64_t> PanViewportAnchor(
    std::int64_t anchor_ns, core::BarRange rendered_range,
    float horizontal_drag_pixels, Rect plot) {
    if (!plot.IsUsable() || rendered_range.end_ns <= rendered_range.start_ns ||
        anchor_ns <= 0 || !Finite(horizontal_drag_pixels))
        return std::nullopt;
    const long double duration =
        static_cast<long double>(rendered_range.end_ns) -
        static_cast<long double>(rendered_range.start_ns);
    const long double delta =
        static_cast<long double>(horizontal_drag_pixels) /
        static_cast<long double>(plot.Width()) * duration;
    return ClampTimestamp(static_cast<long double>(anchor_ns) - delta);
}

core::BarRange ShiftRange(core::BarRange range, std::int64_t delta_ns) {
    return {
        .start_ns = ClampTimestamp(static_cast<long double>(range.start_ns) +
                                   static_cast<long double>(delta_ns)),
        .end_ns = ClampTimestamp(static_cast<long double>(range.end_ns) +
                                 static_cast<long double>(delta_ns)),
    };
}

VisibleIndices SelectVisibleIndices(
    std::span<const core::MarketBar> bars, core::BarRange range) {
    const auto first = std::ranges::lower_bound(
        bars, range.start_ns, {}, &core::MarketBar::start_ns);
    const auto last = std::ranges::lower_bound(
        bars, range.end_ns, {}, &core::MarketBar::start_ns);
    return {
        .first = static_cast<std::size_t>(first - bars.begin()),
        .last = static_cast<std::size_t>(last - bars.begin()),
    };
}

float PriceScale::ToY(double price, Rect plot) const {
    if (!plot.IsUsable() || !Finite(price) || maximum <= minimum)
        return (plot.top + plot.bottom) * 0.5f;
    const double fraction = (maximum - price) / (maximum - minimum);
    return plot.top + static_cast<float>(fraction) * plot.Height();
}

double PriceScale::FromY(float y, Rect plot) const {
    if (!plot.IsUsable() || maximum <= minimum) return minimum;
    const double fraction = std::clamp(
        static_cast<double>(y - plot.top) / plot.Height(), 0.0, 1.0);
    return maximum - fraction * (maximum - minimum);
}

std::optional<PriceScale> AutoscalePrices(
    std::span<const core::MarketBar> bars, VisibleIndices visible,
    const core::MarketBar* current_bar, float padding_fraction) {
    if (visible.Empty() && current_bar == nullptr) return std::nullopt;
    if (!Finite(padding_fraction) || padding_fraction < 0.0f)
        return std::nullopt;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    const auto include = [&](const core::MarketBar& bar) {
        IncludePrice(Display(bar.low), minimum, maximum);
        IncludePrice(Display(bar.high), minimum, maximum);
    };
    const std::size_t last = std::min(visible.last, bars.size());
    for (std::size_t index = std::min(visible.first, last);
         index < last; ++index)
        include(bars[index]);
    if (current_bar != nullptr) include(*current_bar);
    if (!Finite(minimum) || !Finite(maximum)) return std::nullopt;
    if (minimum == maximum) {
        const double padding = std::max(1.0, std::abs(minimum) * 0.05);
        return PriceScale{minimum - padding, maximum + padding};
    }
    const double padding =
        std::max(std::abs(maximum - minimum) * padding_fraction,
                 std::numeric_limits<double>::epsilon());
    return PriceScale{minimum - padding, maximum + padding};
}

std::optional<double> MaximumVolume(
    std::span<const core::MarketBar> bars, VisibleIndices visible,
    const core::MarketBar* current_bar) {
    double maximum = 0.0;
    const auto include = [&](const core::MarketBar& bar) {
        const double volume = Display(bar.volume);
        if (Finite(volume)) maximum = std::max(maximum, volume);
    };
    const std::size_t last = std::min(visible.last, bars.size());
    for (std::size_t index = std::min(visible.first, last);
         index < last; ++index)
        include(bars[index]);
    if (current_bar != nullptr) include(*current_bar);
    return maximum > 0.0 ? std::optional<double>(maximum) : std::nullopt;
}

float TimeToX(std::int64_t time_ns, core::BarRange range, Rect plot) {
    if (!plot.IsUsable() || range.end_ns <= range.start_ns)
        return plot.left;
    const double fraction = std::clamp(
        static_cast<double>(time_ns - range.start_ns) /
            static_cast<double>(range.end_ns - range.start_ns),
        0.0, 1.0);
    return plot.left + static_cast<float>(fraction) * plot.Width();
}

std::int64_t XToTime(float x, core::BarRange range, Rect plot) {
    if (!plot.IsUsable() || range.end_ns <= range.start_ns)
        return range.start_ns;
    const double fraction = std::clamp(
        static_cast<double>(x - plot.left) / plot.Width(), 0.0, 1.0);
    return range.start_ns + static_cast<std::int64_t>(
        fraction * static_cast<double>(range.end_ns - range.start_ns));
}

CandleGeometry MakeCandleGeometry(
    const core::MarketBar& bar, PriceScale scale, core::BarRange range,
    Rect plot, float body_half_width) {
    const double open = Display(bar.open);
    const double close = Display(bar.close);
    return {
        .x = TimeToX(bar.start_ns, range, plot),
        .open_y = scale.ToY(open, plot),
        .close_y = scale.ToY(close, plot),
        .high_y = scale.ToY(Display(bar.high), plot),
        .low_y = scale.ToY(Display(bar.low), plot),
        .body_half_width = std::max(0.5f, body_half_width),
        .rising = close > open,
        .unchanged = close == open,
    };
}

float VolumeToY(double volume, double maximum_volume, Rect plot) {
    if (!plot.IsUsable() || !Finite(volume) || maximum_volume <= 0.0)
        return plot.bottom;
    const double fraction = std::clamp(volume / maximum_volume, 0.0, 1.0);
    return plot.bottom - static_cast<float>(fraction) * plot.Height();
}

std::optional<std::size_t> HitTestBar(
    std::span<const core::MarketBar> bars, VisibleIndices visible,
    std::int64_t time_ns, core::BarRange range) {
    if (visible.Empty() || bars.empty()) return std::nullopt;
    const std::size_t last = std::min(visible.last, bars.size());
    std::size_t nearest = std::min(visible.first, last - 1);
    std::int64_t distance = std::numeric_limits<std::int64_t>::max();
    for (std::size_t index = std::min(visible.first, last);
         index < last; ++index) {
        const std::int64_t current = bars[index].start_ns;
        const std::int64_t next_distance =
            current > time_ns ? current - time_ns : time_ns - current;
        if (next_distance < distance) {
            distance = next_distance;
            nearest = index;
        }
    }
    static_cast<void>(range);
    return nearest;
}

std::vector<core::MarketBar> AggregateBarsByScreenColumn(
    std::span<const core::MarketBar> bars, VisibleIndices visible,
    core::BarRange range, Rect plot) {
    std::vector<core::MarketBar> result;
    if (!plot.IsUsable() || range.end_ns <= range.start_ns || visible.Empty())
        return result;
    const std::size_t last = std::min(visible.last, bars.size());
    const std::size_t first = std::min(visible.first, last);
    result.reserve(std::min(last - first,
                            static_cast<std::size_t>(
                                std::ceil(plot.Width())) + 1U));
    int previous_column = std::numeric_limits<int>::min();
    for (std::size_t index = first; index < last; ++index) {
        const core::MarketBar& bar = bars[index];
        const int column = static_cast<int>(std::floor(
            TimeToX(bar.start_ns, range, plot) - plot.left));
        if (result.empty() || column != previous_column) {
            result.push_back(bar);
            previous_column = column;
            continue;
        }
        core::MarketBar& aggregate = result.back();
        aggregate.high = std::max(aggregate.high, bar.high);
        aggregate.low = std::min(aggregate.low, bar.low);
        aggregate.close = bar.close;
        aggregate.volume += bar.volume;
        const std::uint64_t remaining =
            std::numeric_limits<std::uint64_t>::max() - aggregate.trade_count;
        aggregate.trade_count += std::min(remaining, bar.trade_count);
        aggregate.within_bar_vwap.reset();
        aggregate.source = bar.source;
        aggregate.state = bar.state;
        aggregate.revision = std::max(aggregate.revision, bar.revision);
    }
    return result;
}

}  // namespace tradebox::gui::chart
