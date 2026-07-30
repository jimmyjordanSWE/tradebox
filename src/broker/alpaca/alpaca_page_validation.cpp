#include "tradebox/broker/alpaca_page_validation.h"

#include <nlohmann/json.hpp>

namespace tradebox::broker::alpaca {

std::expected<HistoricalPageEnvelope, std::string>
ValidateHistoricalPage(
    std::string_view body,
    std::string_view array_field) {
    try {
        const auto value =
            nlohmann::json::parse(body.begin(), body.end());
        if (!value.is_object())
            return std::unexpected(
                "historical page is not an object");
        const auto items =
            value.find(std::string(array_field));
        if (items == value.end() || !items->is_array())
            return std::unexpected(
                "historical page is missing array field '" +
                std::string(array_field) + "'");
        for (const auto& item : *items)
            if (!item.is_object())
                return std::unexpected(
                    "historical page contains a non-object item");

        HistoricalPageEnvelope result;
        const auto token = value.find("next_page_token");
        if (token == value.end() || token->is_null())
            return result;
        if (!token->is_string())
            return std::unexpected(
                "historical next_page_token is not a string or null");
        result.next_page_token =
            token->get<std::string>();
        return result;
    } catch (const std::exception& error) {
        return std::unexpected(
            "historical page JSON is malformed: " +
            std::string(error.what()));
    }
}

}  // namespace tradebox::broker::alpaca
