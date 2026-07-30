#pragma once

#include <chrono>

namespace tradebox::broker::alpaca {

inline constexpr auto kAccountReanchorInterval =
    std::chrono::minutes(5);
inline constexpr auto kPositionReanchorInterval =
    std::chrono::minutes(5);
inline constexpr auto kMarketClockInterval =
    std::chrono::minutes(5);
inline constexpr auto kOrderFallbackInterval =
    std::chrono::seconds(5);
inline constexpr auto kActivityRefreshInterval =
    std::chrono::minutes(5);

}  // namespace tradebox::broker::alpaca
