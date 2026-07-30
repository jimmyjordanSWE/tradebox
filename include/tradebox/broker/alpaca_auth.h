#pragma once

#include <string>
#include <string_view>

namespace tradebox::broker::alpaca {

[[nodiscard]] std::string BuildAuthenticationMessage(
    std::string_view api_key, std::string_view api_secret);

}  // namespace tradebox::broker::alpaca
