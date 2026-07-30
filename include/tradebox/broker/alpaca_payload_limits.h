#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace tradebox::broker::alpaca {

inline constexpr std::size_t kMaximumRestResponseBytes =
    64U * 1024U * 1024U;
inline constexpr std::size_t kMaximumMarketStreamMessageBytes =
    8U * 1024U * 1024U;
inline constexpr std::size_t kMaximumAccountStreamMessageBytes =
    8U * 1024U * 1024U;

[[nodiscard]] inline bool AppendInboundPayload(
    std::string& destination, std::string_view chunk,
    std::size_t maximum_bytes) {
    if (destination.size() > maximum_bytes ||
        chunk.size() > maximum_bytes - destination.size())
        return false;
    destination.append(chunk);
    return true;
}

}  // namespace tradebox::broker::alpaca
