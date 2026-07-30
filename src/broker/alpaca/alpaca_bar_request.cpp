#include "tradebox/broker/alpaca_bar_request.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace tradebox::broker::alpaca {
namespace {

void HashCombine(std::size_t& seed, std::size_t value) {
    seed ^= value + 0x9e3779b9U +
            (seed << 6U) + (seed >> 2U);
}

std::string UrlEncode(std::string_view value) {
    std::ostringstream result;
    result << std::uppercase << std::hex;
    for (const unsigned char character : value) {
        if (std::isalnum(character) || character == '-' ||
            character == '_' || character == '.' ||
            character == '~') {
            result << character;
        } else {
            result << '%' << std::setw(2)
                   << std::setfill('0')
                   << static_cast<int>(character);
        }
    }
    return result.str();
}

std::string TimestampNs(std::int64_t timestamp_ns) {
    const std::time_t seconds =
        static_cast<std::time_t>(
            timestamp_ns / 1'000'000'000);
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    std::ostringstream value;
    value << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S");
    const std::int64_t fraction =
        timestamp_ns % 1'000'000'000;
    if (fraction != 0)
        value << '.' << std::setw(9)
              << std::setfill('0') << fraction;
    value << 'Z';
    return value.str();
}

const char* Feed(core::MarketDataFeed feed) {
    return feed == core::MarketDataFeed::Sip ? "sip" : "iex";
}

const char* Adjustment(core::BarAdjustment adjustment) {
    switch (adjustment) {
        case core::BarAdjustment::Split:
            return "split";
        case core::BarAdjustment::Dividend:
            return "dividend";
        case core::BarAdjustment::All:
            return "all";
        case core::BarAdjustment::Raw:
        default:
            return "raw";
    }
}

}  // namespace

std::string BuildHistoricalBarPath(
    const HistoricalBarPageRequest& request) {
    if (request.symbol.empty() ||
        request.key.timeframe.empty() ||
        request.range.start_ns < 0 ||
        request.range.start_ns >= request.range.end_ns)
        return {};
    const std::int64_t inclusive_end =
        request.range.end_ns - 1;
    std::string path =
        "/v2/stocks/" + UrlEncode(request.symbol) +
        "/bars?timeframe=" +
        UrlEncode(request.key.timeframe) +
        "&start=" +
        UrlEncode(TimestampNs(request.range.start_ns)) +
        "&end=" +
        UrlEncode(TimestampNs(inclusive_end)) +
        "&limit=" +
        std::to_string(
            std::clamp<std::size_t>(
                request.limit, 1, 10'000)) +
        "&adjustment=" +
        Adjustment(request.key.adjustment) +
        "&feed=" + Feed(request.key.feed) +
        "&sort=asc";
    if (!request.page_token.empty())
        path += "&page_token=" +
                UrlEncode(request.page_token);
    return path;
}

std::size_t InFlightBarRanges::KeyHash::operator()(
    const core::BarSeriesKey& key) const {
    std::size_t result =
        std::hash<std::string>{}(key.instrument_id);
    HashCombine(result, std::hash<int>{}(
                            static_cast<int>(key.feed)));
    HashCombine(result,
                std::hash<std::string>{}(key.timeframe));
    HashCombine(result, std::hash<int>{}(
                            static_cast<int>(key.adjustment)));
    return result;
}

std::vector<core::BarRange> InFlightBarRanges::Reserve(
    const core::BarSeriesKey& key,
    const std::vector<core::BarRange>& missing) {
    std::scoped_lock lock(mutex_);
    std::vector<core::BarRange>& active = active_[key];
    std::vector<core::BarRange> reserved =
        core::SubtractBarRanges(missing, active);
    for (const core::BarRange range : reserved)
        core::MergeBarRange(active, range);
    if (active.empty()) active_.erase(key);
    return reserved;
}

void InFlightBarRanges::Release(
    const core::BarSeriesKey& key,
    const std::vector<core::BarRange>& ranges) {
    std::scoped_lock lock(mutex_);
    const auto found = active_.find(key);
    if (found == active_.end()) return;
    found->second =
        core::SubtractBarRanges(found->second, ranges);
    if (found->second.empty()) active_.erase(found);
}

}  // namespace tradebox::broker::alpaca
