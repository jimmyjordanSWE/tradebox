#pragma once

#include "tradebox/core/bar_series.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

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

}  // namespace tradebox::gui::chart
