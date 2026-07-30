#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace tradebox::broker::alpaca {

struct HistoricalPageEnvelope {
    std::string next_page_token;
};

[[nodiscard]] std::expected<HistoricalPageEnvelope, std::string>
ValidateHistoricalPage(
    std::string_view body,
    std::string_view array_field);

}  // namespace tradebox::broker::alpaca
