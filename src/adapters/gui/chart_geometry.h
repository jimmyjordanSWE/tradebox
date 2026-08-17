#pragma once

#include "tradebox/core/bar_series.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tradebox::gui::chart {

struct Rect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    [[nodiscard]] float Width() const { return right - left; }
    [[nodiscard]] float Height() const { return bottom - top; }
    [[nodiscard]] bool IsUsable() const {
        return Width() > 0.0f && Height() > 0.0f;
    }
};

struct VisibleIndices {
    std::size_t first = 0;
    std::size_t last = 0;

    [[nodiscard]] bool Empty() const { return first >= last; }
};

struct ViewportState {
    int visible_bars = 120;
    std::int64_t anchor_ns = 0;
};

[[nodiscard]] std::optional<ViewportState> ZoomViewport(
    ViewportState current, core::BarRange rendered_range,
    float pointer_x, Rect plot, float wheel_delta,
    int minimum_visible_bars = 30,
    int maximum_visible_bars = 2'000);
[[nodiscard]] std::optional<std::int64_t> PanViewportAnchor(
    std::int64_t anchor_ns, core::BarRange rendered_range,
    float horizontal_drag_pixels, Rect plot);
[[nodiscard]] core::BarRange ShiftRange(core::BarRange range,
                                        std::int64_t delta_ns);

[[nodiscard]] VisibleIndices SelectVisibleIndices(
    std::span<const core::MarketBar> bars, core::BarRange range);

struct PriceScale {
    double minimum = 0.0;
    double maximum = 1.0;

    [[nodiscard]] float ToY(double price, Rect plot) const;
    [[nodiscard]] double FromY(float y, Rect plot) const;
};

[[nodiscard]] std::optional<PriceScale> AutoscalePrices(
    std::span<const core::MarketBar> bars,
    VisibleIndices visible,
    const core::MarketBar* current_bar = nullptr,
    float padding_fraction = 0.05f);

[[nodiscard]] std::optional<double> MaximumVolume(
    std::span<const core::MarketBar> bars,
    VisibleIndices visible,
    const core::MarketBar* current_bar = nullptr);

[[nodiscard]] float TimeToX(std::int64_t time_ns,
                            core::BarRange range, Rect plot);
[[nodiscard]] std::int64_t XToTime(float x,
                                    core::BarRange range, Rect plot);

struct CandleGeometry {
    float x = 0.0f;
    float open_y = 0.0f;
    float close_y = 0.0f;
    float high_y = 0.0f;
    float low_y = 0.0f;
    float body_half_width = 0.0f;
    bool rising = false;
    bool unchanged = false;
};

[[nodiscard]] CandleGeometry MakeCandleGeometry(
    const core::MarketBar& bar, PriceScale scale, core::BarRange range,
    Rect plot, float body_half_width);

[[nodiscard]] float VolumeToY(double volume, double maximum_volume,
                              Rect plot);

[[nodiscard]] std::optional<std::size_t> HitTestBar(
    std::span<const core::MarketBar> bars,
    VisibleIndices visible,
    std::int64_t time_ns,
    core::BarRange range);

[[nodiscard]] std::vector<core::MarketBar> AggregateBarsByScreenColumn(
    std::span<const core::MarketBar> bars,
    VisibleIndices visible,
    core::BarRange range,
    Rect plot);

}  // namespace tradebox::gui::chart
