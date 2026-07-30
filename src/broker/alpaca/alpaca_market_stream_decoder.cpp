#include "tradebox/broker/alpaca_market_stream_decoder.h"

#include <rapidjson/error/en.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/reader.h>

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace tradebox::broker::alpaca {
namespace {

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

enum class Field {
    Unknown,
    Kind,
    Message,
    Symbol,
    Identifier,
    OriginalIdentifier,
    CorrectedIdentifier,
    Timestamp,
    Exchange,
    BidExchange,
    AskExchange,
    Tape,
    BidPrice,
    BidSize,
    AskPrice,
    AskSize,
    Price,
    Size,
    CorrectedPrice,
    CorrectedSize,
    Open,
    High,
    Low,
    CloseOrConditions,
    Volume,
    Vwap,
    TradeCount,
    Conditions,
    CorrectedConditions,
    StatusCode,
    StatusMessage,
    ReasonCode,
    ReasonMessage,
    TradeSymbols,
    QuoteSymbols,
    StatusSymbols,
};

Field FieldFor(std::string_view key) {
    if (key == "T") return Field::Kind;
    if (key == "msg") return Field::Message;
    if (key == "S") return Field::Symbol;
    if (key == "i") return Field::Identifier;
    if (key == "oi") return Field::OriginalIdentifier;
    if (key == "ci") return Field::CorrectedIdentifier;
    if (key == "t") return Field::Timestamp;
    if (key == "x") return Field::Exchange;
    if (key == "bx") return Field::BidExchange;
    if (key == "ax") return Field::AskExchange;
    if (key == "z") return Field::Tape;
    if (key == "bp") return Field::BidPrice;
    if (key == "bs") return Field::BidSize;
    if (key == "ap") return Field::AskPrice;
    if (key == "as") return Field::AskSize;
    if (key == "p") return Field::Price;
    if (key == "s") return Field::Size;
    if (key == "cp") return Field::CorrectedPrice;
    if (key == "cs") return Field::CorrectedSize;
    if (key == "o") return Field::Open;
    if (key == "h") return Field::High;
    if (key == "l") return Field::Low;
    if (key == "c") return Field::CloseOrConditions;
    if (key == "v") return Field::Volume;
    if (key == "vw") return Field::Vwap;
    if (key == "n") return Field::TradeCount;
    if (key == "cc") return Field::CorrectedConditions;
    if (key == "sc") return Field::StatusCode;
    if (key == "sm") return Field::StatusMessage;
    if (key == "rc") return Field::ReasonCode;
    if (key == "rm") return Field::ReasonMessage;
    if (key == "trades") return Field::TradeSymbols;
    if (key == "quotes") return Field::QuoteSymbols;
    if (key == "statuses") return Field::StatusSymbols;
    return Field::Unknown;
}

enum class ScalarType {
    Missing,
    Null,
    String,
    Signed,
    Unsigned,
    Float,
    Boolean,
};

struct Scalar {
    ScalarType type = ScalarType::Missing;
    std::string text;
};

struct ItemFields {
    Scalar kind;
    Scalar message;
    Scalar symbol;
    Scalar identifier;
    Scalar original_identifier;
    Scalar corrected_identifier;
    Scalar timestamp;
    Scalar exchange;
    Scalar bid_exchange;
    Scalar ask_exchange;
    Scalar tape;
    Scalar bid_price;
    Scalar bid_size;
    Scalar ask_price;
    Scalar ask_size;
    Scalar price;
    Scalar size;
    Scalar corrected_price;
    Scalar corrected_size;
    Scalar open;
    Scalar high;
    Scalar low;
    Scalar close;
    Scalar volume;
    Scalar vwap;
    Scalar trade_count;
    Scalar status_code;
    Scalar status_message;
    Scalar reason_code;
    Scalar reason_message;
    std::vector<std::string> conditions;
    std::vector<std::string> corrected_conditions;
    std::vector<std::string> trade_symbols;
    std::vector<std::string> quote_symbols;
    std::vector<std::string> status_symbols;
};

Scalar* ScalarFor(ItemFields& item, Field field) {
    switch (field) {
        case Field::Kind: return &item.kind;
        case Field::Message: return &item.message;
        case Field::Symbol: return &item.symbol;
        case Field::Identifier: return &item.identifier;
        case Field::OriginalIdentifier:
            return &item.original_identifier;
        case Field::CorrectedIdentifier:
            return &item.corrected_identifier;
        case Field::Timestamp: return &item.timestamp;
        case Field::Exchange: return &item.exchange;
        case Field::BidExchange: return &item.bid_exchange;
        case Field::AskExchange: return &item.ask_exchange;
        case Field::Tape: return &item.tape;
        case Field::BidPrice: return &item.bid_price;
        case Field::BidSize: return &item.bid_size;
        case Field::AskPrice: return &item.ask_price;
        case Field::AskSize: return &item.ask_size;
        case Field::Price: return &item.price;
        case Field::Size: return &item.size;
        case Field::CorrectedPrice: return &item.corrected_price;
        case Field::CorrectedSize: return &item.corrected_size;
        case Field::Open: return &item.open;
        case Field::High: return &item.high;
        case Field::Low: return &item.low;
        case Field::CloseOrConditions: return &item.close;
        case Field::Volume: return &item.volume;
        case Field::Vwap: return &item.vwap;
        case Field::TradeCount: return &item.trade_count;
        case Field::StatusCode: return &item.status_code;
        case Field::StatusMessage: return &item.status_message;
        case Field::ReasonCode: return &item.reason_code;
        case Field::ReasonMessage: return &item.reason_message;
        default: return nullptr;
    }
}

std::vector<std::string>* ArrayFor(ItemFields& item,
                                   Field field) {
    switch (field) {
        case Field::CloseOrConditions:
        case Field::Conditions:
            return &item.conditions;
        case Field::CorrectedConditions:
            return &item.corrected_conditions;
        case Field::TradeSymbols:
            return &item.trade_symbols;
        case Field::QuoteSymbols:
            return &item.quote_symbols;
        case Field::StatusSymbols:
            return &item.status_symbols;
        default:
            return nullptr;
    }
}

std::string StringValue(const Scalar& value) {
    return value.type == ScalarType::String
               ? value.text
               : std::string{};
}

std::string IdentifierValue(const Scalar& value) {
    if (value.type == ScalarType::String ||
        value.type == ScalarType::Signed ||
        value.type == ScalarType::Unsigned)
        return value.text;
    return {};
}

std::string ExpandExponent(std::string_view text) {
    const std::size_t exponent_at = text.find_first_of("eE");
    if (exponent_at == std::string_view::npos)
        return std::string(text);

    int exponent = 0;
    const std::string_view exponent_text =
        text.substr(exponent_at + 1);
    const auto converted = std::from_chars(
        exponent_text.data(),
        exponent_text.data() + exponent_text.size(),
        exponent);
    if (converted.ec != std::errc{} ||
        converted.ptr != exponent_text.data() +
                             exponent_text.size())
        return std::string(text);

    bool negative = false;
    std::size_t cursor = 0;
    if (text[cursor] == '-' || text[cursor] == '+') {
        negative = text[cursor] == '-';
        ++cursor;
    }
    const std::string_view mantissa =
        text.substr(cursor, exponent_at - cursor);
    const std::size_t point = mantissa.find('.');
    const std::size_t digits_before_point =
        point == std::string_view::npos ? mantissa.size()
                                        : point;
    std::string digits;
    digits.reserve(mantissa.size());
    for (const char character : mantissa)
        if (character != '.') digits.push_back(character);

    const std::int64_t decimal_position =
        static_cast<std::int64_t>(digits_before_point) +
        exponent;
    std::string result;
    result.reserve(digits.size() +
                   static_cast<std::size_t>(
                       std::max<std::int64_t>(
                           0, -decimal_position)) +
                   4);
    if (negative) result.push_back('-');
    if (decimal_position <= 0) {
        result += "0.";
        result.append(
            static_cast<std::size_t>(-decimal_position), '0');
        result += digits;
    } else if (static_cast<std::size_t>(decimal_position) >=
               digits.size()) {
        result += digits;
        result.append(
            static_cast<std::size_t>(decimal_position) -
                digits.size(),
            '0');
    } else {
        result.append(
            digits.data(),
            static_cast<std::size_t>(decimal_position));
        result.push_back('.');
        result.append(
            digits.data() + decimal_position,
            digits.size() -
                static_cast<std::size_t>(decimal_position));
    }
    return result;
}

tradebox::core::Decimal DecimalValue(
    const Scalar& value, std::string_view key) {
    if (value.type == ScalarType::Missing ||
        value.type == ScalarType::Null)
        return tradebox::core::Decimal::Zero();
    if (value.type != ScalarType::String &&
        value.type != ScalarType::Signed &&
        value.type != ScalarType::Unsigned &&
        value.type != ScalarType::Float)
        throw std::runtime_error(
            "Expected decimal field: " + std::string(key));
    const std::string normalized =
        value.type == ScalarType::Float
            ? ExpandExponent(value.text)
            : value.text;
    auto parsed = tradebox::core::Decimal::Parse(normalized);
    if (!parsed)
        throw std::runtime_error(
            "Invalid decimal field: " + std::string(key));
    return std::move(*parsed);
}

std::uint64_t UnsignedValue(const Scalar& value) {
    const std::string text = IdentifierValue(value);
    if (text.empty()) return 0;
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} &&
                   parsed.ptr == text.data() + text.size()
               ? result
               : 0;
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

class MarketFrameSax {
public:
    MarketFrameSax(
        std::int64_t received_at_ms,
        const InstrumentResolver& resolve_instrument)
        : received_at_ms_(received_at_ms),
          resolve_instrument_(resolve_instrument) {
        result_.controls.reserve(2);
    }

    bool null() {
        return SetScalar(ScalarType::Null, {});
    }

    bool boolean(bool value) {
        return SetScalar(
            ScalarType::Boolean, value ? "true" : "false");
    }

    bool number_integer(std::int64_t value) {
        return SetScalar(
            ScalarType::Signed, std::to_string(value));
    }

    bool number_unsigned(std::uint64_t value) {
        return SetScalar(
            ScalarType::Unsigned, std::to_string(value));
    }

    bool number_float(
        double, const std::string& token) {
        return SetScalar(ScalarType::Float, token);
    }

    bool string(std::string& value) {
        if (item_active_ && depth_ == 3 &&
            active_array_) {
            active_array_->push_back(std::move(value));
            return true;
        }
        return SetScalar(
            ScalarType::String, std::move(value));
    }

    bool start_object(std::size_t) {
        if (depth_ == 0) {
            root_is_array_ = false;
            return false;
        }
        if (root_is_array_ && depth_ == 1) {
            item_ = {};
            item_active_ = true;
            current_field_ = Field::Unknown;
        }
        ++depth_;
        return true;
    }

    bool key(const std::string& value) {
        if (item_active_ && depth_ == 2)
            current_field_ = FieldFor(value);
        return true;
    }

    bool end_object() {
        if (item_active_ && depth_ == 2) {
            FinalizeItem();
            item_active_ = false;
            current_field_ = Field::Unknown;
        }
        if (depth_ == 0) return false;
        --depth_;
        return true;
    }

    bool start_array(std::size_t elements) {
        if (depth_ == 0) {
            root_is_array_ = true;
            root_closed_ = false;
            result_.items.reserve(
                elements == std::size_t(-1) ? 0 : elements);
            ++depth_;
            return true;
        }
        if (item_active_ && depth_ == 2)
            active_array_ = ArrayFor(item_, current_field_);
        else if (root_is_array_ && depth_ == 1)
            ++result_.ignored_items;
        if (active_array_ && depth_ == 2) {
            active_array_->clear();
            if (Scalar* scalar =
                    ScalarFor(item_, current_field_))
                *scalar = {};
        }
        ++depth_;
        return true;
    }

    bool end_array() {
        if (depth_ == 0) return false;
        if (item_active_ && depth_ == 3)
            active_array_ = nullptr;
        --depth_;
        if (depth_ == 0) root_closed_ = true;
        return true;
    }

    [[nodiscard]] bool Complete() const {
        return root_is_array_ && root_closed_ &&
               depth_ == 0;
    }

    DecodedMarketFrame TakeResult() {
        return std::move(result_);
    }

private:
    bool SetScalar(ScalarType type, std::string text) {
        if (root_is_array_ && depth_ == 1) {
            ++result_.ignored_items;
            return true;
        }
        if (!item_active_ || depth_ != 2) return true;
        if (std::vector<std::string>* array =
                ArrayFor(item_, current_field_))
            array->clear();
        Scalar* destination =
            ScalarFor(item_, current_field_);
        if (destination)
            *destination = {
                .type = type,
                .text = std::move(text),
            };
        return true;
    }

    void FinalizeItem() {
        const std::string kind = StringValue(item_.kind);
        if (kind == "success") {
            if (StringValue(item_.message) == "authenticated")
                result_.controls.push_back({
                    .type = StreamControlType::Authenticated,
                    .message = "authenticated",
                });
            else
                ++result_.ignored_items;
            return;
        }
        if (kind == "error") {
            result_.controls.push_back({
                .type = StreamControlType::Error,
                .message = StringValue(item_.message),
            });
            return;
        }
        if (kind == "subscription") {
            result_.controls.push_back({
                .type = StreamControlType::Subscription,
                .trade_symbols = std::move(item_.trade_symbols),
                .quote_symbols = std::move(item_.quote_symbols),
                .status_symbols = std::move(item_.status_symbols),
            });
            return;
        }
        if (kind != "t" && kind != "q" && kind != "x" &&
            kind != "c" && kind != "b" &&
            kind != "d" && kind != "u" && kind != "s") {
            ++result_.ignored_items;
            return;
        }

        DecodedStreamItem decoded{
            .market_tick =
                kind == "t" || kind == "q" ||
                kind == "x" || kind == "c",
            .kind = kind,
            .symbol = StringValue(item_.symbol),
            .received_at_ms = received_at_ms_,
        };
        const std::string broker_timestamp =
            StringValue(item_.timestamp);
        decoded.event_time_ns =
            ParseTimestampNs(broker_timestamp);
        const std::string identifier =
            IdentifierValue(item_.identifier);
        if (kind == "t")
            decoded.source_event_id =
                decoded.symbol + ":" +
                std::to_string(decoded.event_time_ns) + ":" +
                identifier;

        const std::string instrument_id =
            resolve_instrument_(decoded.symbol);
        if (kind == "s") {
            const std::string status_code =
                StringValue(item_.status_code);
            decoded.source_event_id =
                decoded.symbol + ":s:" +
                std::to_string(decoded.event_time_ns) + ":" +
                status_code + ":" +
                StringValue(item_.reason_code);
            decoded.market_event =
                tradebox::core::ShareMarketDataEvent(
                    tradebox::core::TradingStatusReceived{
                        .status = {
                            .instrument_id = instrument_id,
                            .symbol = decoded.symbol,
                            .state = TradingState(status_code),
                            .status_code = status_code,
                            .status_message =
                                StringValue(item_.status_message),
                            .reason_code =
                                StringValue(item_.reason_code),
                            .reason_message =
                                StringValue(item_.reason_message),
                            .tape = StringValue(item_.tape),
                            .broker_timestamp = broker_timestamp,
                            .event_time_ns =
                                decoded.event_time_ns,
                            .received_at_ms = received_at_ms_,
                        },
                    });
        } else if (kind == "q") {
            decoded.market_event =
                tradebox::core::ShareMarketDataEvent(
                    tradebox::core::QuoteReceived{
                    .quote = {
                        .instrument_id = instrument_id,
                        .symbol = decoded.symbol,
                        .bid_price =
                            DecimalValue(item_.bid_price, "bp"),
                        .bid_size =
                            DecimalValue(item_.bid_size, "bs"),
                        .bid_exchange =
                            StringValue(item_.bid_exchange),
                        .ask_price =
                            DecimalValue(item_.ask_price, "ap"),
                        .ask_size =
                            DecimalValue(item_.ask_size, "as"),
                        .ask_exchange =
                            StringValue(item_.ask_exchange),
                        .conditions = std::move(item_.conditions),
                        .tape = StringValue(item_.tape),
                        .broker_timestamp = broker_timestamp,
                        .event_time_ns = decoded.event_time_ns,
                        .received_at_ms = received_at_ms_,
                    },
                });
        } else if (kind == "t") {
            decoded.market_event =
                tradebox::core::ShareMarketDataEvent(
                    tradebox::core::TradeReceived{
                    .trade = {
                        .instrument_id = instrument_id,
                        .symbol = decoded.symbol,
                        .trade_id = identifier,
                        .price = DecimalValue(item_.price, "p"),
                        .size = DecimalValue(item_.size, "s"),
                        .exchange = StringValue(item_.exchange),
                        .conditions = std::move(item_.conditions),
                        .tape = StringValue(item_.tape),
                        .broker_timestamp = broker_timestamp,
                        .event_time_ns = decoded.event_time_ns,
                        .received_at_ms = received_at_ms_,
                    },
                });
        } else if (kind == "x") {
            decoded.market_event =
                tradebox::core::ShareMarketDataEvent(
                    tradebox::core::TradeCanceled{
                    .instrument_id = instrument_id,
                    .symbol = decoded.symbol,
                    .trade_id = identifier,
                    .broker_timestamp = broker_timestamp,
                    .event_time_ns = decoded.event_time_ns,
                    .received_at_ms = received_at_ms_,
                });
        } else if (kind == "c") {
            decoded.market_event =
                tradebox::core::ShareMarketDataEvent(
                    tradebox::core::TradeCorrected{
                    .instrument_id = instrument_id,
                    .symbol = decoded.symbol,
                    .original_trade_id =
                        IdentifierValue(
                            item_.original_identifier),
                    .corrected_trade = {
                        .instrument_id = instrument_id,
                        .symbol = decoded.symbol,
                        .trade_id =
                            IdentifierValue(
                                item_.corrected_identifier),
                        .price =
                            DecimalValue(
                                item_.corrected_price, "cp"),
                        .size =
                            DecimalValue(
                                item_.corrected_size, "cs"),
                        .exchange =
                            StringValue(item_.exchange),
                        .conditions =
                            std::move(
                                item_.corrected_conditions),
                        .tape = StringValue(item_.tape),
                        .broker_timestamp = broker_timestamp,
                        .event_time_ns = decoded.event_time_ns,
                        .received_at_ms = received_at_ms_,
                    },
                });
        } else {
            decoded.bar = DecodedBar{
                .kind = kind,
                .instrument_id = instrument_id,
                .symbol = decoded.symbol,
                .timestamp_ms =
                    decoded.event_time_ns / 1'000'000,
                .open = DecimalValue(item_.open, "o"),
                .high = DecimalValue(item_.high, "h"),
                .low = DecimalValue(item_.low, "l"),
                .close = DecimalValue(item_.close, "c"),
                .volume = DecimalValue(item_.volume, "v"),
                .within_bar_vwap =
                    item_.vwap.type != ScalarType::Missing &&
                            item_.vwap.type != ScalarType::Null
                        ? std::optional<core::Decimal>(
                              DecimalValue(item_.vwap, "vw"))
                        : std::nullopt,
                .trade_count =
                    UnsignedValue(item_.trade_count),
            };
        }
        result_.items.push_back(std::move(decoded));
    }

    std::int64_t received_at_ms_ = 0;
    const InstrumentResolver& resolve_instrument_;
    DecodedMarketFrame result_;
    ItemFields item_;
    Field current_field_ = Field::Unknown;
    std::vector<std::string>* active_array_ = nullptr;
    std::size_t depth_ = 0;
    bool root_is_array_ = false;
    bool root_closed_ = false;
    bool item_active_ = false;
};

class RapidJsonSaxAdapter {
public:
    explicit RapidJsonSaxAdapter(MarketFrameSax& destination)
        : destination_(destination) {}

    bool Null() { return destination_.null(); }
    bool Bool(bool value) {
        return destination_.boolean(value);
    }
    bool Int(int value) {
        return destination_.number_integer(value);
    }
    bool Uint(unsigned value) {
        return destination_.number_unsigned(value);
    }
    bool Int64(std::int64_t value) {
        return destination_.number_integer(value);
    }
    bool Uint64(std::uint64_t value) {
        return destination_.number_unsigned(value);
    }
    bool Double(double value) {
        return destination_.number_float(value, {});
    }
    bool RawNumber(const char* value,
                   rapidjson::SizeType length, bool) {
        const std::string_view token(value, length);
        if (token.find_first_of(".eE") !=
            std::string_view::npos) {
            double parsed = 0;
            const auto converted = std::from_chars(
                token.data(), token.data() + token.size(),
                parsed, std::chars_format::general);
            if (converted.ec != std::errc{} ||
                converted.ptr !=
                    token.data() + token.size())
                return false;
            return destination_.number_float(
                parsed, std::string(token));
        }
        if (!token.empty() && token.front() == '-') {
            std::int64_t parsed = 0;
            const auto converted = std::from_chars(
                token.data(), token.data() + token.size(),
                parsed);
            return converted.ec == std::errc{} &&
                   converted.ptr ==
                       token.data() + token.size() &&
                   destination_.number_integer(parsed);
        }
        std::uint64_t parsed = 0;
        const auto converted = std::from_chars(
            token.data(), token.data() + token.size(),
            parsed);
        return converted.ec == std::errc{} &&
               converted.ptr ==
                   token.data() + token.size() &&
               destination_.number_unsigned(parsed);
    }
    bool String(const char* value,
                rapidjson::SizeType length, bool) {
        std::string text(value, length);
        return destination_.string(text);
    }
    bool Key(const char* value,
             rapidjson::SizeType length, bool) {
        std::string text(value, length);
        return destination_.key(text);
    }
    bool StartObject() {
        return destination_.start_object(
            std::size_t(-1));
    }
    bool EndObject(rapidjson::SizeType) {
        return destination_.end_object();
    }
    bool StartArray() {
        return destination_.start_array(
            std::size_t(-1));
    }
    bool EndArray(rapidjson::SizeType) {
        return destination_.end_array();
    }

private:
    MarketFrameSax& destination_;
};

class DirectJsonReader {
public:
    DirectJsonReader(
        std::string_view input, MarketFrameSax& destination)
        : input_(input), destination_(destination) {}

    bool Parse() {
        SkipWhitespace();
        if (!ParseValue(0)) return false;
        SkipWhitespace();
        return cursor_ == input_.size() &&
               destination_.Complete();
    }

private:
    void SkipWhitespace() {
        while (cursor_ < input_.size()) {
            const char value = input_[cursor_];
            if (value != ' ' && value != '\t' &&
                value != '\r' && value != '\n')
                break;
            ++cursor_;
        }
    }

    bool Consume(char expected) {
        SkipWhitespace();
        if (cursor_ >= input_.size() ||
            input_[cursor_] != expected)
            return false;
        ++cursor_;
        return true;
    }

    bool ParseString(std::string& result) {
        SkipWhitespace();
        if (cursor_ >= input_.size() ||
            input_[cursor_] != '"')
            return false;
        const std::size_t start = ++cursor_;
        while (cursor_ < input_.size()) {
            const unsigned char value =
                static_cast<unsigned char>(input_[cursor_]);
            if (value == '"') {
                result.assign(
                    input_.data() + start,
                    cursor_ - start);
                ++cursor_;
                return true;
            }
            // Escapes and non-ASCII input use the fully validating
            // Generic SAX fallback.
            if (value == '\\' || value < 0x20 ||
                value >= 0x80)
                return false;
            ++cursor_;
        }
        return false;
    }

    bool ParseObject(std::size_t depth) {
        if (!destination_.start_object(
                std::size_t(-1)))
            return false;
        ++cursor_;
        SkipWhitespace();
        if (cursor_ < input_.size() &&
            input_[cursor_] == '}') {
            ++cursor_;
            return destination_.end_object();
        }
        for (;;) {
            std::string key;
            if (!ParseString(key) ||
                !destination_.key(key) ||
                !Consume(':') ||
                !ParseValue(depth + 1))
                return false;
            SkipWhitespace();
            if (cursor_ < input_.size() &&
                input_[cursor_] == '}') {
                ++cursor_;
                return destination_.end_object();
            }
            if (!Consume(',')) return false;
        }
    }

    bool ParseArray(std::size_t depth) {
        if (!destination_.start_array(
                std::size_t(-1)))
            return false;
        ++cursor_;
        SkipWhitespace();
        if (cursor_ < input_.size() &&
            input_[cursor_] == ']') {
            ++cursor_;
            return destination_.end_array();
        }
        for (;;) {
            if (!ParseValue(depth + 1)) return false;
            SkipWhitespace();
            if (cursor_ < input_.size() &&
                input_[cursor_] == ']') {
                ++cursor_;
                return destination_.end_array();
            }
            if (!Consume(',')) return false;
        }
    }

    bool ParseLiteral(
        std::string_view literal, bool value,
        bool is_null) {
        if (input_.substr(cursor_, literal.size()) !=
            literal)
            return false;
        cursor_ += literal.size();
        return is_null ? destination_.null()
                       : destination_.boolean(value);
    }

    bool ParseNumber() {
        const std::size_t start = cursor_;
        bool negative = false;
        if (input_[cursor_] == '-') {
            negative = true;
            if (++cursor_ == input_.size()) return false;
        }
        if (input_[cursor_] == '0') {
            ++cursor_;
            if (cursor_ < input_.size() &&
                input_[cursor_] >= '0' &&
                input_[cursor_] <= '9')
                return false;
        } else {
            if (input_[cursor_] < '1' ||
                input_[cursor_] > '9')
                return false;
            while (cursor_ < input_.size() &&
                   input_[cursor_] >= '0' &&
                   input_[cursor_] <= '9')
                ++cursor_;
        }
        bool floating = false;
        if (cursor_ < input_.size() &&
            input_[cursor_] == '.') {
            floating = true;
            ++cursor_;
            const std::size_t fraction = cursor_;
            while (cursor_ < input_.size() &&
                   input_[cursor_] >= '0' &&
                   input_[cursor_] <= '9')
                ++cursor_;
            if (cursor_ == fraction) return false;
        }
        if (cursor_ < input_.size() &&
            (input_[cursor_] == 'e' ||
             input_[cursor_] == 'E')) {
            floating = true;
            ++cursor_;
            if (cursor_ < input_.size() &&
                (input_[cursor_] == '+' ||
                 input_[cursor_] == '-'))
                ++cursor_;
            const std::size_t exponent = cursor_;
            while (cursor_ < input_.size() &&
                   input_[cursor_] >= '0' &&
                   input_[cursor_] <= '9')
                ++cursor_;
            if (cursor_ == exponent) return false;
        }
        const std::string_view token =
            input_.substr(start, cursor_ - start);
        if (floating) {
            double parsed = 0;
            const auto converted = std::from_chars(
                token.data(), token.data() + token.size(),
                parsed, std::chars_format::general);
            if (converted.ec != std::errc{} ||
                converted.ptr !=
                    token.data() + token.size())
                return false;
            const std::string token_text(token);
            return destination_.number_float(
                parsed, token_text);
        }
        if (negative) {
            std::int64_t parsed = 0;
            const auto converted = std::from_chars(
                token.data(), token.data() + token.size(),
                parsed);
            return converted.ec == std::errc{} &&
                   converted.ptr ==
                       token.data() + token.size() &&
                   destination_.number_integer(parsed);
        }
        std::uint64_t parsed = 0;
        const auto converted = std::from_chars(
            token.data(), token.data() + token.size(),
            parsed);
        return converted.ec == std::errc{} &&
               converted.ptr ==
                   token.data() + token.size() &&
               destination_.number_unsigned(parsed);
    }

    bool ParseValue(std::size_t depth) {
        constexpr std::size_t kMaximumDepth = 64;
        if (depth > kMaximumDepth) return false;
        SkipWhitespace();
        if (cursor_ >= input_.size()) return false;
        switch (input_[cursor_]) {
            case '{':
                return ParseObject(depth);
            case '[':
                return ParseArray(depth);
            case '"': {
                std::string value;
                return ParseString(value) &&
                       destination_.string(value);
            }
            case 'n':
                return ParseLiteral("null", false, true);
            case 't':
                return ParseLiteral("true", true, false);
            case 'f':
                return ParseLiteral("false", false, false);
            default:
                if (input_[cursor_] == '-' ||
                    (input_[cursor_] >= '0' &&
                     input_[cursor_] <= '9'))
                    return ParseNumber();
                return false;
        }
    }

    std::string_view input_;
    MarketFrameSax& destination_;
    std::size_t cursor_ = 0;
};

DecodedMarketFrame DecodeWithRapidJson(
    std::string_view raw_frame,
    std::int64_t received_at_ms,
    const InstrumentResolver& resolve_instrument) {
    MarketFrameSax decoder(
        received_at_ms, resolve_instrument);
    RapidJsonSaxAdapter adapter(decoder);
    rapidjson::MemoryStream stream(
        raw_frame.data(), raw_frame.size());
    rapidjson::Reader reader;
    constexpr unsigned kFlags =
        rapidjson::kParseNumbersAsStringsFlag |
        rapidjson::kParseValidateEncodingFlag;
    const bool parsed = reader.Parse<kFlags>(
        stream, adapter);
    if (!parsed || !decoder.Complete()) {
        std::string message =
            "market stream JSON is invalid";
        if (!parsed) {
            message += " at byte " +
                       std::to_string(
                           reader.GetErrorOffset()) +
                       ": " +
                       rapidjson::GetParseError_En(
                           reader.GetParseErrorCode());
        } else {
            message += ": packet is not an array";
        }
        throw std::runtime_error(std::move(message));
    }
    return decoder.TakeResult();
}

}  // namespace

DecodedMarketFrame DecodeMarketFrame(
    std::string_view raw_frame, std::int64_t received_at_ms,
    const InstrumentResolver& resolve_instrument,
    MarketJsonBackend backend) {
    if (backend ==
        MarketJsonBackend::DirectWithRapidFallback) {
        MarketFrameSax direct_decoder(
            received_at_ms, resolve_instrument);
        if (DirectJsonReader(
                raw_frame, direct_decoder).Parse())
            return direct_decoder.TakeResult();
    }
    return DecodeWithRapidJson(
        raw_frame, received_at_ms,
        resolve_instrument);
}

}  // namespace tradebox::broker::alpaca
