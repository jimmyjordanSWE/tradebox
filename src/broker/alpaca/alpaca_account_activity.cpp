#include "tradebox/broker/alpaca_account_activity.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace tradebox::broker::alpaca {
namespace {

using json = nlohmann::json;

std::string String(const json& value, const char* key) {
    if (!value.contains(key) || !value[key].is_string()) return {};
    return value[key].get<std::string>();
}

std::string Identifier(const json& value, const char* key) {
    if (!value.contains(key) || value[key].is_null()) return {};
    if (value[key].is_string()) return value[key].get<std::string>();
    if (value[key].is_number_integer())
        return std::to_string(value[key].get<std::int64_t>());
    if (value[key].is_number_unsigned())
        return std::to_string(value[key].get<std::uint64_t>());
    return {};
}

std::optional<tradebox::core::Decimal> Decimal(
    const json& value, const char* key) {
    if (!value.contains(key) || value[key].is_null()) return std::nullopt;
    const std::string text =
        value[key].is_string() ? value[key].get<std::string>()
                               : value[key].dump();
    auto parsed = tradebox::core::Decimal::Parse(text);
    if (!parsed)
        throw std::runtime_error(
            std::string("Invalid decimal field ") + key);
    return *parsed;
}

std::int64_t TimestampMs(const std::string& value) {
    if (value.size() < 10) return 0;
    std::tm time{};
    std::istringstream stream(
        value.size() >= 19 ? value.substr(0, 19)
                           : value.substr(0, 10) + "T00:00:00");
    stream >> std::get_time(&time, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) return 0;
    std::int64_t result =
        static_cast<std::int64_t>(_mkgmtime64(&time)) * 1000;
    const std::size_t dot = value.find('.');
    if (dot != std::string::npos) {
        std::string fraction;
        for (std::size_t index = dot + 1;
             index < value.size() && fraction.size() < 3; ++index) {
            if (!std::isdigit(
                    static_cast<unsigned char>(value[index])))
                break;
            fraction.push_back(value[index]);
        }
        while (fraction.size() < 3) fraction.push_back('0');
        if (!fraction.empty()) result += std::stoi(fraction);
    }
    return result;
}

std::string Encode(std::string_view value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char character : value) {
        if (std::isalnum(character) || character == '-' ||
            character == '_' || character == '.' || character == '~') {
            encoded << character;
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<int>(character);
        }
    }
    return encoded.str();
}

}  // namespace

std::string BuildAccountActivitiesPath(
    std::string_view page_token, std::string_view after) {
    std::string path =
        "/v2/account/activities?direction=desc&page_size=100";
    if (!page_token.empty())
        path += "&page_token=" + Encode(page_token);
    if (!after.empty()) path += "&after=" + Encode(after);
    return path;
}

std::expected<ParsedAccountActivityPage, std::string>
ParseAccountActivityPage(
    std::string_view raw_payload, std::string_view fallback_account_id,
    const std::vector<tradebox::core::OrderState>& authoritative_orders,
    std::string_view current_page_token, std::size_t page_size) {
    try {
        const json value = json::parse(raw_payload);
        if (!value.is_array())
            return std::unexpected(
                "Account activities response is not an array");
        ParsedAccountActivityPage page;
        page.activities.reserve(value.size());
        for (const auto& item : value) {
            if (!item.is_object())
                return std::unexpected(
                    "Account activity item is not an object");
            const json details =
                item.contains("details") && item["details"].is_object()
                    ? item["details"]
                    : json::object();
            tradebox::core::AccountActivity activity;
            activity.account_id = String(item, "account_id");
            if (activity.account_id.empty())
                activity.account_id = std::string(fallback_account_id);
            activity.provider_id = Identifier(item, "id");
            if (activity.provider_id.empty())
                activity.provider_id = Identifier(item, "event_id");
            if (activity.provider_id.empty())
                activity.provider_id = Identifier(item, "ref_id");
            if (activity.account_id.empty() ||
                activity.provider_id.empty())
                return std::unexpected(
                    "Account activity lacks account or provider identity");
            activity.activity_type = String(item, "activity_type");
            activity.activity_subtype =
                String(item, "activity_subtype");
            activity.execution_type = String(item, "type");
            if (activity.execution_type.empty())
                activity.execution_type =
                    String(details, "execution_type");
            activity.status = String(item, "status");
            activity.symbol = String(item, "symbol");
            if (activity.symbol.empty())
                activity.symbol = String(details, "symbol");
            activity.cusip = String(item, "cusip");
            if (activity.cusip.empty())
                activity.cusip = String(details, "cusip");
            activity.side = String(item, "side");
            activity.order_id = Identifier(item, "order_id");
            if (activity.order_id.empty())
                activity.order_id = Identifier(details, "order_id");
            activity.currency = String(item, "currency");
            activity.occurred_at = String(item, "transaction_time");
            if (activity.occurred_at.empty())
                activity.occurred_at = String(item, "at");
            if (activity.occurred_at.empty())
                activity.occurred_at = String(item, "date");
            activity.settlement_date = String(item, "settle_date");
            activity.occurred_at_ms =
                TimestampMs(activity.occurred_at);
            activity.qty = Decimal(item, "qty");
            activity.price = Decimal(item, "price");
            activity.cumulative_qty = Decimal(item, "cum_qty");
            activity.leaves_qty = Decimal(item, "leaves_qty");
            activity.net_amount = Decimal(item, "net_amount");
            activity.per_share_amount =
                Decimal(item, "per_share_amount");
            activity.raw_payload = item.dump();

            const bool fill =
                activity.activity_type == "FILL" ||
                activity.activity_type == "TRD";
            if (fill) {
                const auto order = std::ranges::find_if(
                    authoritative_orders,
                    [&activity](
                        const tradebox::core::OrderState& candidate) {
                        return candidate.id == activity.order_id;
                    });
                if (order == authoritative_orders.end()) {
                    activity.fill_reconciliation =
                        tradebox::core::ActivityFillReconciliation::
                            OrderNotFound;
                } else {
                    const auto expected = activity.cumulative_qty
                                              ? activity.cumulative_qty
                                              : activity.qty;
                    if (expected &&
                        order->filled_qty < *expected &&
                        activity.execution_type != "trade_bust" &&
                        activity.execution_type != "trade_correct") {
                        activity.fill_reconciliation =
                            tradebox::core::ActivityFillReconciliation::
                                QuantityMismatch;
                    } else {
                        activity.fill_reconciliation =
                            tradebox::core::ActivityFillReconciliation::
                                MatchedOrder;
                    }
                }
            }
            page.activities.push_back(std::move(activity));
        }
        if (value.size() == page_size && !page.activities.empty()) {
            page.next_page_token =
                page.activities.back().provider_id;
            if (page.next_page_token == current_page_token)
                return std::unexpected(
                    "Account activity pagination token repeated");
        }
        return page;
    } catch (const std::exception& error) {
        return std::unexpected(
            "Account activities JSON error: " +
            std::string(error.what()));
    }
}

}  // namespace tradebox::broker::alpaca
