#pragma once

#include "tradebox/core/account_activity.h"
#include "tradebox/core/types.h"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tradebox::broker::alpaca {

struct ParsedAccountActivityPage {
    std::vector<tradebox::core::AccountActivity> activities;
    std::string next_page_token;
};

std::string BuildAccountActivitiesPath(
    std::string_view page_token = {}, std::string_view after = {});

std::expected<ParsedAccountActivityPage, std::string>
ParseAccountActivityPage(
    std::string_view raw_payload, std::string_view fallback_account_id,
    const std::vector<tradebox::core::OrderState>& authoritative_orders,
    std::string_view current_page_token = {},
    std::size_t page_size = 100);

}  // namespace tradebox::broker::alpaca
