#include "tradebox/broker/alpaca_market_stream_decoder.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace tradebox::broker::alpaca {
namespace {

using json = nlohmann::json;

int Digits(std::string_view value, std::size_t offset,
           std::size_t count) {
    if (offset + count > value.size()) return -1;
    int result = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char digit = value[offset + index];
        if (digit < '0' || digit > '9') return -1;
        result = result * 10 + digit - '0';
    }
    return result;
}

constexpr bool LeapYear(int year) {
    return year % 4 == 0 &&
           (year % 100 != 0 || year % 400 == 0);
}

constexpr int DaysInMonth(int year, int month) {
    constexpr int days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31,
    };
    return days[month - 1] +
           (month == 2 && LeapYear(year) ? 1 : 0);
}

constexpr std::int64_t DaysFromCivil(int year, unsigned month,
                                     unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era =
        static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153 * (month > 2 ? month - 3 : month + 9) + 2) /
            5 +
        day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 -
        year_of_era / 100 + day_of_year;
    return static_cast<std::int64_t>(era) * 146097 +
           day_of_era - 719468;
}

std::int64_t ParseTimestampNs(std::string_view value) {
    if (value.size() < 19 || value[4] != '-' ||
        value[7] != '-' ||
        (value[10] != 'T' && value[10] != 't') ||
        value[13] != ':' || value[16] != ':')
        return 0;
    const int year = Digits(value, 0, 4);
    const int month = Digits(value, 5, 2);
    const int day = Digits(value, 8, 2);
    const int hour = Digits(value, 11, 2);
    const int minute = Digits(value, 14, 2);
    const int second = Digits(value, 17, 2);
    if (year < 1 || month < 1 || month > 12 || day < 1 ||
        day > DaysInMonth(year, month) || hour < 0 ||
        hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59)
        return 0;

    std::size_t cursor = 19;
    std::int64_t fractional_ns = 0;
    int fractional_digits = 0;
    if (cursor < value.size() && value[cursor] == '.') {
        ++cursor;
        while (cursor < value.size() &&
               value[cursor] >= '0' && value[cursor] <= '9') {
            if (fractional_digits < 9) {
                fractional_ns =
                    fractional_ns * 10 + value[cursor] - '0';
                ++fractional_digits;
            }
            ++cursor;
        }
        while (fractional_digits++ < 9) fractional_ns *= 10;
    }

    int offset_seconds = 0;
    if (cursor < value.size() &&
        (value[cursor] == 'Z' || value[cursor] == 'z')) {
        ++cursor;
    } else if (cursor < value.size() &&
               (value[cursor] == '+' || value[cursor] == '-')) {
        const bool positive = value[cursor] == '+';
        if (cursor + 6 != value.size() ||
            value[cursor + 3] != ':')
            return 0;
        const int offset_hours =
            Digits(value, cursor + 1, 2);
        const int offset_minutes =
            Digits(value, cursor + 4, 2);
        if (offset_hours < 0 || offset_hours > 23 ||
            offset_minutes < 0 || offset_minutes > 59)
            return 0;
        offset_seconds =
            (offset_hours * 60 + offset_minutes) * 60;
        if (positive) offset_seconds = -offset_seconds;
        cursor += 6;
    }
    if (cursor != value.size()) return 0;

    const std::int64_t seconds_since_epoch =
        DaysFromCivil(year, static_cast<unsigned>(month),
                      static_cast<unsigned>(day)) *
            86'400 +
        hour * 3'600 + minute * 60 + second +
        offset_seconds;
    return seconds_since_epoch * 1'000'000'000 +
           fractional_ns;
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

tradebox::core::SecurityTradingState TradingState(
    std::string_view code) {
    if (code == "2" || code == "H")
        return tradebox::core::SecurityTradingState::Halted;
    if (code == "P")
        return tradebox::core::SecurityTradingState::Paused;
    if (code == "3" || code == "Q" || code == "T")
        return tradebox::core::SecurityTradingState::Trading;
    return tradebox::core::SecurityTradingState::Unknown;
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
                .status_symbols = StringArray(item, "statuses"),
            });
            continue;
        }
        if (kind != "t" && kind != "q" && kind != "x" &&
            kind != "c" && kind != "b" &&
            kind != "d" && kind != "u" && kind != "s") {
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
        if (kind == "s") {
            const std::string status_code =
                String(item, "sc");
            decoded.source_event_id =
                decoded.symbol + ":s:" +
                std::to_string(decoded.event_time_ns) + ":" +
                status_code + ":" + String(item, "rc");
            decoded.market_event =
                tradebox::core::ShareMarketDataEvent(
                    tradebox::core::TradingStatusReceived{
                        .status = {
                            .instrument_id = instrument_id,
                            .symbol = decoded.symbol,
                            .state = TradingState(status_code),
                            .status_code = status_code,
                            .status_message = String(item, "sm"),
                            .reason_code = String(item, "rc"),
                            .reason_message = String(item, "rm"),
                            .tape = String(item, "z"),
                            .broker_timestamp = broker_timestamp,
                            .event_time_ns =
                                decoded.event_time_ns,
                            .received_at_ms = received_at_ms,
                        },
                    });
        } else if (kind == "q") {
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
