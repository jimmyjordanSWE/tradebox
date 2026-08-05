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

}  // namespace

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

}  // namespace tradebox::gui::chart
