#pragma once

#include "tradebox/core/decimal.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tradebox::core {

enum class ActivityFillReconciliation {
    NotApplicable,
    MatchedOrder,
    OrderNotFound,
    QuantityMismatch,
};

struct AccountActivity {
    std::string account_id;
    std::string provider_id;
    std::string activity_type;
    std::string activity_subtype;
    std::string execution_type;
    std::string status;
    std::string symbol;
    std::string cusip;
    std::string side;
    std::string order_id;
    std::string currency;
    std::string occurred_at;
    std::string settlement_date;
    std::int64_t occurred_at_ms = 0;
    std::optional<Decimal> qty;
    std::optional<Decimal> price;
    std::optional<Decimal> cumulative_qty;
    std::optional<Decimal> leaves_qty;
    std::optional<Decimal> net_amount;
    std::optional<Decimal> per_share_amount;
    ActivityFillReconciliation fill_reconciliation =
        ActivityFillReconciliation::NotApplicable;
    std::string raw_payload;
    std::uint32_t revision = 1;
};

struct AccountActivityQuery {
    std::string account_id;
    std::string activity_type;
    std::string symbol;
    std::int64_t after_ms = 0;
    std::int64_t before_ms = 0;
    std::int64_t cursor_time_ms = 0;
    std::string cursor_provider_id;
    std::size_t maximum = 250;
};

struct AccountActivityPage {
    std::vector<AccountActivity> activities;
    std::int64_t next_cursor_time_ms = 0;
    std::string next_cursor_provider_id;
};

struct AccountActivityWriteResult {
    std::size_t inserted = 0;
    std::size_t revised = 0;
    std::size_t unchanged = 0;
};

}  // namespace tradebox::core
