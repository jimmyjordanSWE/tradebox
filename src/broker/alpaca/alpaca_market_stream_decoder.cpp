#include "tradebox/broker/alpaca_market_stream_decoder.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace tradebox::broker::alpaca {
namespace {

using json = nlohmann::json;

std::int64_t ParseTimestampMs(const std::string& value) {
    if (value.size() < 19) return 0;
    std::tm time{};
    std::istringstream stream(value.substr(0, 19));
    stream >> std::get_time(&time, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) return 0;
    std::int64_t milliseconds =
        static_cast<std::int64_t>(_mkgmtime64(&time)) * 1'000;
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
        if (!fraction.empty()) milliseconds += std::stoi(fraction);
    }
    const std::size_t offset_at =
        value.find_first_of("+-", 19);
    if (offset_at != std::string::npos &&
        offset_at + 5 < value.size()) {
        try {
            const int hours =
                std::stoi(value.substr(offset_at + 1, 2));
            const int minutes =
                std::stoi(value.substr(offset_at + 4, 2));
            const std::int64_t offset_ms =
                static_cast<std::int64_t>(hours * 60 + minutes) *
                60 * 1'000;
            milliseconds +=
                value[offset_at] == '+' ? -offset_ms : offset_ms;
        } catch (...) {
        }
    }
    return milliseconds;
}

std::int64_t ParseTimestampNs(const std::string& value) {
    const std::int64_t milliseconds = ParseTimestampMs(value);
    if (milliseconds == 0) return 0;
    std::int64_t sub_millisecond_ns = 0;
    const std::size_t dot = value.find('.');
    if (dot != std::string::npos) {
        std::string fraction;
        for (std::size_t index = dot + 1;
             index < value.size() && fraction.size() < 9; ++index) {
            if (!std::isdigit(
                    static_cast<unsigned char>(value[index])))
                break;
            fraction.push_back(value[index]);
        }
        while (fraction.size() < 9) fraction.push_back('0');
        if (fraction.size() == 9)
            sub_millisecond_ns =
                std::stoll(fraction.substr(3, 6));
    }
    return milliseconds * 1'000'000 + sub_millisecond_ns;
}

std::string String(const json& object, const char* key) {
    if (!object.contains(key) || object[key].is_null() ||
        !object[key].is_string())
        return {};
    return object[key].get<std::string>();
}

std::string Identifier(const json& object, const char* key) {
    if (!object.contains(key) || object[key].is_null()) return {};
    if (object[key].is_string())
        return object[key].get<std::string>();
    if (object[key].is_number_integer())
        return std::to_string(object[key].get<std::int64_t>());
    if (object[key].is_number_unsigned())
        return std::to_string(object[key].get<std::uint64_t>());
    return {};
}

std::uint64_t Unsigned(const json& object, const char* key) {
    const std::string value = Identifier(object, key);
    if (value.empty()) return 0;
    try {
        return std::stoull(value);
    } catch (...) {
        return 0;
    }
}

std::vector<std::string> StringArray(
    const json& object, const char* key) {
    std::vector<std::string> result;
    if (!object.contains(key) || !object[key].is_array())
        return result;
    for (const auto& value : object[key])
        if (value.is_string())
            result.push_back(value.get<std::string>());
    return result;
}

tradebox::core::Decimal Decimal(
    const json& object, const char* key) {
    if (!object.contains(key) || object[key].is_null())
        return tradebox::core::Decimal::Zero();
    std::string text;
    if (object[key].is_string())
        text = object[key].get<std::string>();
    else if (object[key].is_number())
        text = object[key].dump();
    else
        throw std::runtime_error(
            std::string("Expected decimal field: ") + key);
    auto parsed = tradebox::core::Decimal::Parse(text);
    if (!parsed)
        throw std::runtime_error(
            std::string("Invalid decimal field: ") + key);
    return std::move(*parsed);
}

}  // namespace

DecodedMarketFrame DecodeMarketFrame(
    std::string_view raw_frame, std::int64_t received_at_ms,
    const InstrumentResolver& resolve_instrument) {
    const json packet =
        json::parse(raw_frame.begin(), raw_frame.end());
    if (!packet.is_array())
        throw std::runtime_error(
            "market stream packet is not an array");

    DecodedMarketFrame result;
    result.controls.reserve(2);
    result.items.reserve(packet.size());
    for (const auto& item : packet) {
        const std::string kind = String(item, "T");
        if (kind == "success") {
            if (String(item, "msg") == "authenticated")
                result.controls.push_back({
                    .type = StreamControlType::Authenticated,
                    .message = "authenticated",
                });
            else
                ++result.ignored_items;
            continue;
        }
        if (kind == "error") {
            result.controls.push_back({
                .type = StreamControlType::Error,
                .message = String(item, "msg"),
            });
            continue;
        }
        if (kind == "subscription") {
            result.controls.push_back({
                .type = StreamControlType::Subscription,
                .trade_symbols = StringArray(item, "trades"),
                .quote_symbols = StringArray(item, "quotes"),
            });
            continue;
        }
        if (kind != "t" && kind != "q" && kind != "x" &&
            kind != "c" && kind != "b" &&
            kind != "d" && kind != "u") {
            ++result.ignored_items;
            continue;
        }

        DecodedStreamItem decoded{
            .market_tick =
                kind == "t" || kind == "q" ||
                kind == "x" || kind == "c",
            .kind = kind,
            .symbol = String(item, "S"),
            .received_at_ms = received_at_ms,
        };
        const std::string broker_timestamp = String(item, "t");
        decoded.event_time_ns =
            ParseTimestampNs(broker_timestamp);
        if (kind == "t" && item.contains("i"))
            decoded.source_event_id =
                decoded.symbol + ":" +
                std::to_string(decoded.event_time_ns) + ":" +
                Identifier(item, "i");

        const std::string instrument_id =
            resolve_instrument(decoded.symbol);
        if (kind == "q") {
            decoded.market_event =
                tradebox::core::ShareMarketDataEvent(
                    tradebox::core::QuoteReceived{
                    .quote = {
                        .instrument_id = instrument_id,
                        .symbol = decoded.symbol,
                        .bid_price = Decimal(item, "bp"),
                        .bid_size = Decimal(item, "bs"),
                        .bid_exchange = String(item, "bx"),
                        .ask_price = Decimal(item, "ap"),
                        .ask_size = Decimal(item, "as"),
                        .ask_exchange = String(item, "ax"),
                        .conditions = StringArray(item, "c"),
                        .tape = String(item, "z"),
                        .broker_timestamp = broker_timestamp,
                        .event_time_ns = decoded.event_time_ns,
                        .received_at_ms = received_at_ms,
                    },
                });
        } else if (kind == "t") {
            decoded.market_event =
                tradebox::core::ShareMarketDataEvent(
                    tradebox::core::TradeReceived{
                    .trade = {
                        .instrument_id = instrument_id,
                        .symbol = decoded.symbol,
                        .trade_id = Identifier(item, "i"),
                        .price = Decimal(item, "p"),
                        .size = Decimal(item, "s"),
                        .exchange = String(item, "x"),
                        .conditions = StringArray(item, "c"),
                        .tape = String(item, "z"),
                        .broker_timestamp = broker_timestamp,
                        .event_time_ns = decoded.event_time_ns,
                        .received_at_ms = received_at_ms,
                    },
                });
        } else if (kind == "x") {
            decoded.market_event =
                tradebox::core::ShareMarketDataEvent(
                    tradebox::core::TradeCanceled{
                    .instrument_id = instrument_id,
                    .symbol = decoded.symbol,
                    .trade_id = Identifier(item, "i"),
                    .broker_timestamp = broker_timestamp,
                    .event_time_ns = decoded.event_time_ns,
                    .received_at_ms = received_at_ms,
                });
        } else if (kind == "c") {
            decoded.market_event =
                tradebox::core::ShareMarketDataEvent(
                    tradebox::core::TradeCorrected{
                    .instrument_id = instrument_id,
                    .symbol = decoded.symbol,
                    .original_trade_id = Identifier(item, "oi"),
                    .corrected_trade = {
                        .instrument_id = instrument_id,
                        .symbol = decoded.symbol,
                        .trade_id = Identifier(item, "ci"),
                        .price = Decimal(item, "cp"),
                        .size = Decimal(item, "cs"),
                        .exchange = String(item, "x"),
                        .conditions = StringArray(item, "cc"),
                        .tape = String(item, "z"),
                        .broker_timestamp = broker_timestamp,
                        .event_time_ns = decoded.event_time_ns,
                        .received_at_ms = received_at_ms,
                    },
                });
        } else {
            decoded.bar = DecodedBar{
                .kind = kind,
                .instrument_id = instrument_id,
                .symbol = decoded.symbol,
                .timestamp_ms =
                    decoded.event_time_ns / 1'000'000,
                .open = Decimal(item, "o"),
                .high = Decimal(item, "h"),
                .low = Decimal(item, "l"),
                .close = Decimal(item, "c"),
                .volume = Decimal(item, "v"),
                .within_bar_vwap =
                    item.contains("vw") &&
                            !item["vw"].is_null()
                        ? std::optional<core::Decimal>(
                              Decimal(item, "vw"))
                        : std::nullopt,
                .trade_count = Unsigned(item, "n"),
            };
        }
        result.items.push_back(std::move(decoded));
    }
    return result;
}

}  // namespace tradebox::broker::alpaca
