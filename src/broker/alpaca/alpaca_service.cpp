#include "tradebox/broker/alpaca_service.h"
#include "tradebox/broker/alpaca_market_stream_decoder.h"
#include "tradebox/broker/alpaca_order_codec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

using json = nlohmann::json;

namespace {

struct HttpResult {
    DWORD status = 0;
    std::string body;
    std::string error;
};

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

std::int64_t ParseTimestampMs(const std::string& value) {
    if (value.size() < 19) return 0;
    std::tm time{};
    std::istringstream stream(value.substr(0, 19));
    stream >> std::get_time(&time, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) return 0;
    std::int64_t milliseconds = static_cast<std::int64_t>(_mkgmtime64(&time)) * 1000;
    const std::size_t dot = value.find('.');
    if (dot != std::string::npos) {
        std::string fraction;
        for (std::size_t i = dot + 1; i < value.size() && fraction.size() < 3;
             ++i) {
            if (!std::isdigit(static_cast<unsigned char>(value[i]))) break;
            fraction.push_back(value[i]);
        }
        while (fraction.size() < 3) fraction.push_back('0');
        if (!fraction.empty()) milliseconds += std::stoi(fraction);
    }
    std::size_t offset_at = value.find_first_of("+-", 19);
    if (offset_at != std::string::npos && offset_at + 5 < value.size()) {
        try {
            const int hours = std::stoi(value.substr(offset_at + 1, 2));
            const int minutes = std::stoi(value.substr(offset_at + 4, 2));
            const std::int64_t offset_ms =
                static_cast<std::int64_t>(hours * 60 + minutes) * 60 * 1000;
            milliseconds += value[offset_at] == '+' ? -offset_ms : offset_ms;
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

double Number(const json& object, const char* key) {
    if (!object.contains(key) || object[key].is_null()) return 0;
    if (object[key].is_number()) return object[key].get<double>();
    if (object[key].is_string()) {
        try {
            return std::stod(object[key].get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

std::string String(const json& object, const char* key) {
    if (!object.contains(key) || object[key].is_null() ||
        !object[key].is_string())
        return {};
    return object[key].get<std::string>();
}

std::string Identifier(const json& object, const char* key) {
    if (!object.contains(key) || object[key].is_null()) return {};
    if (object[key].is_string()) return object[key].get<std::string>();
    if (object[key].is_number_integer())
        return std::to_string(object[key].get<std::int64_t>());
    if (object[key].is_number_unsigned())
        return std::to_string(object[key].get<std::uint64_t>());
    return {};
}

std::vector<std::string> StringArray(const json& object,
                                     const char* key) {
    std::vector<std::string> result;
    if (!object.contains(key) || !object[key].is_array()) return result;
    for (const auto& value : object[key])
        if (value.is_string()) result.push_back(value.get<std::string>());
    return result;
}

std::optional<std::string> DecimalText(const json& object, const char* key) {
    if (!object.contains(key) || object[key].is_null()) return std::nullopt;
    if (object[key].is_string()) return object[key].get<std::string>();
    if (object[key].is_number_integer())
        return std::to_string(object[key].get<std::int64_t>());
    if (object[key].is_number_unsigned())
        return std::to_string(object[key].get<std::uint64_t>());
    if (object[key].is_number_float()) return object[key].dump();
    throw std::runtime_error(std::string("Expected decimal field: ") + key);
}

tradebox::core::Decimal RequiredDecimal(const json& object, const char* key) {
    const std::optional<std::string> text = DecimalText(object, key);
    if (!text) return tradebox::core::Decimal::Zero();
    auto parsed = tradebox::core::Decimal::Parse(*text);
    if (!parsed)
        throw std::runtime_error(std::string("Invalid decimal ") + key +
                                 ": " + parsed.error().message);
    return std::move(*parsed);
}

std::optional<tradebox::core::Decimal> OptionalDecimal(
    const json& object, const char* key) {
    const std::optional<std::string> text = DecimalText(object, key);
    if (!text) return std::nullopt;
    auto parsed = tradebox::core::Decimal::Parse(*text);
    if (!parsed)
        throw std::runtime_error(std::string("Invalid decimal ") + key +
                                 ": " + parsed.error().message);
    return std::move(*parsed);
}

std::int64_t WallClockNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string TimestampNs(std::int64_t timestamp_ns) {
    const std::time_t seconds =
        static_cast<std::time_t>(timestamp_ns / 1'000'000'000);
    const std::int64_t nanoseconds =
        timestamp_ns % 1'000'000'000;
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    std::ostringstream value;
    value << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S")
          << '.' << std::setw(9) << std::setfill('0')
          << nanoseconds << 'Z';
    return value.str();
}

HttpResult Request(const wchar_t* method, const std::wstring& host,
                   const std::wstring& path,
                   const AlpacaCredentials& credentials,
                   std::string_view body = {}) {
    HttpResult result;
    HINTERNET session =
        WinHttpOpen(L"TradeBoxNative/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        result.error = "WinHttpOpen failed";
        return result;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    HINTERNET connection = WinHttpConnect(session, host.c_str(),
                                          INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request =
        connection ? WinHttpOpenRequest(connection, method, path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE)
                   : nullptr;
    const std::wstring headers =
        L"APCA-API-KEY-ID: " + Wide(credentials.key) +
        L"\r\nAPCA-API-SECRET-KEY: " + Wide(credentials.secret) +
        L"\r\nContent-Type: application/json\r\n";
    void* request_body =
        body.empty() ? WINHTTP_NO_REQUEST_DATA
                     : const_cast<char*>(body.data());
    if (!request ||
        !WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L),
                            request_body, static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        result.error = "HTTPS request failed (" + std::to_string(GetLastError()) + ")";
    } else {
        DWORD size = sizeof(result.status);
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &result.status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
                break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
            result.body.append(chunk.data(), read);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

HttpResult Get(const std::wstring& host, const std::wstring& path,
               const AlpacaCredentials& credentials) {
    return Request(L"GET", host, path, credentials);
}

const char* AlpacaFeedName(
    tradebox::core::MarketDataFeed feed) {
    return feed == tradebox::core::MarketDataFeed::Sip
               ? "sip"
               : "iex";
}

std::wstring HistoryPath(
    const std::string& symbol,
    const std::string& timeframe,
    tradebox::core::MarketDataFeed feed) {
    const auto now = std::chrono::system_clock::now();
    const auto start = now - std::chrono::hours(24 * 365 * 6);
    const std::time_t start_time = std::chrono::system_clock::to_time_t(start);
    std::tm utc{};
    gmtime_s(&utc, &start_time);
    std::ostringstream date;
    date << std::put_time(&utc, "%Y-%m-%d");
    return Wide("/v2/stocks/" + symbol +
                "/bars?timeframe=" + timeframe + "&start=" + date.str() +
                "&limit=1000&adjustment=all&feed=" +
                AlpacaFeedName(feed) + "&sort=desc");
}

std::string UrlEncode(const std::string& value) {
    std::ostringstream result;
    result << std::uppercase << std::hex;
    for (const unsigned char character : value) {
        if (std::isalnum(character) || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            result << character;
        } else {
            result << '%' << std::setw(2) << std::setfill('0')
                   << static_cast<int>(character);
        }
    }
    return result.str();
}

std::string StablePayloadId(std::string_view payload) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char value : payload) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    std::ostringstream result;
    result << std::hex << std::setw(16) << std::setfill('0')
           << hash;
    return result.str();
}

tradebox::core::OrderState ParseCoreOrder(const json& value) {
    tradebox::core::OrderState order;
    order.id = String(value, "id");
    order.client_order_id = String(value, "client_order_id");
    order.asset_id = String(value, "asset_id");
    order.symbol = String(value, "symbol");
    order.asset_class = String(value, "asset_class");
    order.side = String(value, "side");
    order.type = String(value, "type");
    if (order.type.empty()) order.type = String(value, "order_type");
    order.time_in_force = String(value, "time_in_force");
    order.order_class = String(value, "order_class");
    order.status = String(value, "status");
    order.submitted_at = String(value, "submitted_at");
    order.updated_at = String(value, "updated_at");
    order.filled_at = String(value, "filled_at");
    order.canceled_at = String(value, "canceled_at");
    order.expired_at = String(value, "expired_at");
    order.failed_at = String(value, "failed_at");
    order.replaced_at = String(value, "replaced_at");
    order.replaced_by = String(value, "replaced_by");
    order.replaces = String(value, "replaces");
    order.qty = OptionalDecimal(value, "qty");
    order.notional = OptionalDecimal(value, "notional");
    order.filled_qty = RequiredDecimal(value, "filled_qty");
    order.filled_avg_price = OptionalDecimal(value, "filled_avg_price");
    order.limit_price = OptionalDecimal(value, "limit_price");
    order.stop_price = OptionalDecimal(value, "stop_price");
    order.extended_hours = value.value("extended_hours", false);
    order.submitted_at_ms = ParseTimestampMs(order.submitted_at);
    order.updated_at_ms = ParseTimestampMs(order.updated_at);
    return order;
}

bool SendText(HINTERNET socket, const std::string& message) {
    return WinHttpWebSocketSend(
               socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
               const_cast<char*>(message.data()),
               static_cast<DWORD>(message.size())) == NO_ERROR;
}

std::string Subscription(const char* action,
                         const std::vector<std::string>& symbols) {
    json value = {
        {"action", action},
        {"trades", symbols},
        {"quotes", symbols},
        {"bars", symbols},
        {"dailyBars", symbols},
        {"updatedBars", symbols},
        {"corrections", symbols},
        {"cancelErrors", symbols},
    };
    return value.dump();
}

tradebox::broker::BrokerCommandResult CommandResult(
    const HttpResult& response) {
    tradebox::broker::BrokerCommandResult result{
        .http_status = response.status,
        .raw_response = response.body,
    };
    if (!response.error.empty() || response.status == 0 ||
        response.status == 408 || response.status == 429 ||
        response.status >= 500) {
        result.disposition =
            tradebox::broker::BrokerCommandDisposition::Indeterminate;
        result.message = response.error.empty()
                             ? "Broker outcome is indeterminate"
                             : response.error;
        return result;
    }
    if (response.status >= 200 && response.status < 300) {
        result.disposition =
            tradebox::broker::BrokerCommandDisposition::Accepted;
        result.message = "Broker accepted HTTP command";
        if (!response.body.empty()) {
            try {
                const json value = json::parse(response.body);
                result.broker_order_id = value.value("id", "");
            } catch (const std::exception&) {
                result.disposition =
                    tradebox::broker::BrokerCommandDisposition::Indeterminate;
                result.message =
                    "Broker returned success with an unreadable response";
            }
        }
        return result;
    }
    result.disposition =
        tradebox::broker::BrokerCommandDisposition::Rejected;
    result.message = response.body.empty() ? "Broker rejected HTTP command"
                                           : response.body;
    return result;
}

}  // namespace

AlpacaService::AlpacaService(UiEventQueue& events, Database& database,
                             tradebox::core::ITradingCore& core,
                             tradebox::core::IMarketDataSink& market_data,
                             tradebox::core::IMarketDataView& market_data_view,
                             tradebox::core::IBarDataSink& bars)
    : events_(events),
      database_(database),
      core_(core),
      market_data_(market_data),
      market_data_view_(market_data_view),
      bars_(bars) {
    for (const auto& asset : database_.LoadAssetCatalog()) {
        if (!asset.symbol.empty() && !asset.instrument_id.empty())
            instrument_ids_by_symbol_.insert_or_assign(
                asset.symbol, asset.instrument_id);
    }
}

AlpacaService::~AlpacaService() {
    Disconnect();
}

AlpacaCredentials AlpacaService::CredentialsSnapshot() const {
    std::scoped_lock lock(credentials_mutex_);
    return credentials_;
}

std::string AlpacaService::InstrumentIdForSymbol(
    const std::string& symbol) const {
    std::scoped_lock lock(asset_catalog_mutex_);
    const auto found = instrument_ids_by_symbol_.find(symbol);
    return found == instrument_ids_by_symbol_.end()
               ? std::string{}
               : found->second;
}

void AlpacaService::Connect(AlpacaCredentials credentials,
                            const std::vector<std::string>& symbols,
                            tradebox::core::MarketDataFeed feed) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    Disconnect();
    const std::uint64_t generation = generation_counter_.fetch_add(1) + 1;
    active_generation_ = generation;
    PublishCoreEvent(tradebox::core::BrokerEvent{
        .kind = tradebox::core::BrokerEventKind::ConnectionAttemptStarted,
        .generation = tradebox::core::ConnectionGeneration{generation},
        .source_event_id =
            "connection:" + std::to_string(generation),
        .raw_payload = "{}",
    });
    {
        std::scoped_lock lock(credentials_mutex_);
        credentials_ = std::move(credentials);
    }
    market_data_feed_ = feed;
    {
        std::scoped_lock lock(subscription_mutex_);
        desired_symbols_ = symbols;
        subscribed_symbols_.clear();
    }
    running_ = true;
    {
        std::scoped_lock lock(workers_mutex_);
        orders_dirty_ = true;
        positions_dirty_ = true;
        workers_.emplace_back(&AlpacaService::AccountRefreshLoop, this);
        workers_.emplace_back(&AlpacaService::MarketClockLoop, this);
    }
    stream_thread_ = std::thread(&AlpacaService::StreamLoop, this, symbols);
    account_stream_thread_ =
        std::thread(&AlpacaService::AccountStreamLoop, this);
}

void AlpacaService::RefreshSymbols(const std::vector<std::string>& symbols) {
    if (!running_) return;
    std::vector<std::string> previous;
    {
        std::scoped_lock lock(subscription_mutex_);
        desired_symbols_ = symbols;
        previous = subscribed_symbols_;
        subscribed_symbols_ = symbols;
    }
    HINTERNET socket = websocket_.load();
    if (!socket || !connected_) return;
    std::scoped_lock send_lock(websocket_send_mutex_);
    if (!previous.empty())
        SendText(socket, Subscription("unsubscribe", previous));
    if (!symbols.empty())
        SendText(socket, Subscription("subscribe", symbols));
    if (!symbols.empty()) {
        std::scoped_lock worker_lock(workers_mutex_);
        workers_.emplace_back(
            &AlpacaService::SeedLatestSnapshots, this, symbols);
    }
}

void AlpacaService::RequestHistory(const std::string& symbol,
                                   const std::string& timeframe) {
    if (!running_) return;
    std::scoped_lock lock(workers_mutex_);
    workers_.emplace_back(&AlpacaService::FetchHistory, this, symbol, timeframe);
}

void AlpacaService::RequestAssetCatalog() {
    if (!running_) return;
    std::scoped_lock lock(workers_mutex_);
    workers_.emplace_back(&AlpacaService::FetchAssetCatalog, this);
}

std::future<tradebox::core::TickSeries> AlpacaService::RequestTicks(
    tradebox::core::TickQuery query) {
    auto promise =
        std::make_shared<std::promise<tradebox::core::TickSeries>>();
    std::future<tradebox::core::TickSeries> result =
        promise->get_future();
    {
        std::scoped_lock lock(workers_mutex_);
        workers_.emplace_back(
            [this, promise, query = std::move(query)]() {
                try {
                    promise->set_value(FetchTicks(query));
                } catch (const std::exception& error) {
                    tradebox::core::TickSeries failed{.query = query};
                    failed.error = error.what();
                    promise->set_value(std::move(failed));
                }
            });
    }
    return result;
}

tradebox::core::TickSeries AlpacaService::FetchTicks(
    const tradebox::core::TickQuery& query) {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    tradebox::core::TickSeries result{.query = query};
    if (query.symbol.empty() || query.start_ns < 0 ||
        query.end_ns <= query.start_ns) {
        result.error = "Invalid tick query";
        return result;
    }
    if (credentials.key.empty() || credentials.secret.empty()) {
        result = database_.LoadMarketTicks(query);
        result.missing_trade_ranges =
            query.include_trades
                ? database_.MissingMarketTickCoverage(query, "t")
                : std::vector<tradebox::core::TickCoverage>{};
        result.missing_quote_ranges =
            query.include_quotes
                ? database_.MissingMarketTickCoverage(query, "q")
                : std::vector<tradebox::core::TickCoverage>{};
        result.complete = result.missing_trade_ranges.empty() &&
                          result.missing_quote_ranges.empty();
        if (!result.complete)
            result.error =
                "No market-data credentials available for missing ranges";
        return result;
    }
    const std::string feed = AlpacaFeedName(query.feed);
    const std::string instrument_id =
        InstrumentIdForSymbol(query.symbol);
    const auto fetch_kind =
        [&](std::string_view kind,
            const std::vector<tradebox::core::TickCoverage>& ranges) {
            const std::string resource =
                kind == "t" ? "trades" : "quotes";
            for (const auto& range : ranges) {
                std::string page_token;
                bool succeeded = true;
                do {
                    std::string path =
                        "/v2/stocks/" + UrlEncode(query.symbol) + "/" +
                        resource + "?start=" +
                        UrlEncode(TimestampNs(range.start_ns)) + "&end=" +
                        UrlEncode(TimestampNs(range.end_ns)) +
                        "&limit=10000&sort=asc&feed=" + feed;
                    if (!page_token.empty())
                        path += "&page_token=" + UrlEncode(page_token);
                    const HttpResult response =
                        Get(L"data.alpaca.markets", Wide(path), credentials);
                    if (response.status != 200) {
                        succeeded = false;
                        result.error =
                            "Historical " + resource + " failed: " +
                            (!response.error.empty() ? response.error
                                                     : response.body);
                        break;
                    }
                    const json body = json::parse(response.body);
                    const auto resource_items = body.find(resource);
                    if (resource_items != body.end() &&
                        resource_items->is_array()) {
                        for (const auto& item : *resource_items) {
                            const std::string timestamp =
                                String(item, "t");
                            const std::int64_t event_time_ns =
                                ParseTimestampNs(timestamp);
                            const std::int64_t received_at_ms =
                                WallClockNowMs();
                            std::string source_id;
                            tradebox::core::MarketDataEventPtr event;
                            if (kind == "t") {
                                tradebox::core::TradeReceived typed{
                                    .trade = {
                                        .instrument_id =
                                            instrument_id,
                                        .symbol = query.symbol,
                                        .trade_id =
                                            Identifier(item, "i"),
                                        .price =
                                            RequiredDecimal(item, "p"),
                                        .size =
                                            RequiredDecimal(item, "s"),
                                        .exchange = String(item, "x"),
                                        .conditions =
                                            StringArray(item, "c"),
                                        .tape = String(item, "z"),
                                        .broker_timestamp = timestamp,
                                        .event_time_ns = event_time_ns,
                                        .received_at_ms = received_at_ms,
                                    },
                                };
                                source_id =
                                    query.symbol + ":" +
                                    std::to_string(event_time_ns) + ":" +
                                    typed.trade.trade_id;
                                event =
                                    tradebox::core::
                                        ShareMarketDataEvent(
                                            std::move(typed));
                            } else {
                                tradebox::core::QuoteReceived typed{
                                    .quote = {
                                        .instrument_id =
                                            instrument_id,
                                        .symbol = query.symbol,
                                        .bid_price =
                                            RequiredDecimal(item, "bp"),
                                        .bid_size =
                                            RequiredDecimal(item, "bs"),
                                        .bid_exchange =
                                            String(item, "bx"),
                                        .ask_price =
                                            RequiredDecimal(item, "ap"),
                                        .ask_size =
                                            RequiredDecimal(item, "as"),
                                        .ask_exchange =
                                            String(item, "ax"),
                                        .conditions =
                                            StringArray(item, "c"),
                                        .tape = String(item, "z"),
                                        .broker_timestamp = timestamp,
                                        .event_time_ns = event_time_ns,
                                        .received_at_ms = received_at_ms,
                                    },
                                };
                                const std::string identity =
                                    timestamp + "|" +
                                    typed.quote.bid_price.ToString() +
                                    "|" +
                                    typed.quote.ask_price.ToString() +
                                    "|" +
                                    typed.quote.bid_size.ToString() +
                                    "|" +
                                    typed.quote.ask_size.ToString();
                                source_id =
                                    query.symbol + ":q:" +
                                    std::to_string(event_time_ns) + ":" +
                                    StablePayloadId(identity);
                                event =
                                    tradebox::core::
                                        ShareMarketDataEvent(
                                            std::move(typed));
                            }
                            database_.QueueMarketDataEvent(
                                feed, std::move(source_id),
                                std::move(event));
                        }
                    }
                    database_.FlushQueuedWrites();
                    // Alpaca returns JSON null, rather than an empty string,
                    // for the final page.
                    page_token = String(body, "next_page_token");
                } while (!page_token.empty());
                if (succeeded)
                    database_.MarkMarketTickCoverage(
                        query, kind, range);
            }
        };
    if (query.include_trades)
        fetch_kind(
            "t", database_.MissingMarketTickCoverage(query, "t"));
    if (query.include_quotes)
        fetch_kind(
            "q", database_.MissingMarketTickCoverage(query, "q"));

    result = database_.LoadMarketTicks(query);
    const auto live = market_data_view_.Snapshot(query.symbol);
    std::unordered_map<std::string, std::size_t> trade_ids;
    for (std::size_t index = 0; index < result.trades.size(); ++index)
        if (!result.trades[index].trade_id.empty())
            trade_ids[result.trades[index].trade_id] = index;
    for (const auto& trade : live.trades) {
        if (!query.include_trades ||
            trade.event_time_ns < query.start_ns ||
            trade.event_time_ns > query.end_ns)
            continue;
        const auto found = trade_ids.find(trade.trade_id);
        if (!trade.trade_id.empty() && found != trade_ids.end())
            result.trades[found->second] = trade;
        else
            result.trades.push_back(trade);
    }
    std::ranges::sort(
        result.trades, {},
        &tradebox::core::MarketTrade::event_time_ns);
    if (query.include_quotes && live.latest_quote &&
        live.latest_quote->event_time_ns >= query.start_ns &&
        live.latest_quote->event_time_ns <= query.end_ns &&
        (result.quotes.empty() ||
         result.quotes.back().event_time_ns <
             live.latest_quote->event_time_ns))
        result.quotes.push_back(*live.latest_quote);
    result.missing_trade_ranges =
        query.include_trades
            ? database_.MissingMarketTickCoverage(query, "t")
            : std::vector<tradebox::core::TickCoverage>{};
    result.missing_quote_ranges =
        query.include_quotes
            ? database_.MissingMarketTickCoverage(query, "q")
            : std::vector<tradebox::core::TickCoverage>{};
    result.complete = result.missing_trade_ranges.empty() &&
                      result.missing_quote_ranges.empty();
    return result;
}

void AlpacaService::SeedLatestSnapshots(
    std::vector<std::string> symbols) {
    if (symbols.empty()) return;
    const AlpacaCredentials credentials = CredentialsSnapshot();
    std::string joined;
    for (const std::string& symbol : symbols) {
        if (!joined.empty()) joined += ',';
        joined += symbol;
    }
    const std::string feed =
        AlpacaFeedName(market_data_feed_);
    const HttpResult response = Get(
        L"data.alpaca.markets",
        Wide("/v2/stocks/snapshots?symbols=" +
             UrlEncode(joined) + "&feed=" + feed),
        credentials);
    if (response.status != 200) {
        events_.Push({
            UiEventType::Status, {},
            "Market snapshot seed failed: " +
                (!response.error.empty() ? response.error
                                         : response.body)});
        return;
    }
    try {
        const json snapshots = json::parse(response.body);
        for (const std::string& symbol : symbols) {
            if (!snapshots.contains(symbol) ||
                !snapshots[symbol].is_object())
                continue;
            const json& snapshot = snapshots[symbol];
            const std::string instrument_id =
                InstrumentIdForSymbol(symbol);
            if (snapshot.contains("latestQuote") &&
                snapshot["latestQuote"].is_object()) {
                const json& item = snapshot["latestQuote"];
                const std::string timestamp =
                    String(item, "t");
                const std::int64_t event_time_ns =
                    ParseTimestampNs(timestamp);
                const std::int64_t received_at_ms =
                    WallClockNowMs();
                auto event =
                    tradebox::core::ShareMarketDataEvent(
                        tradebox::core::QuoteReceived{
                            .quote = {
                                .instrument_id = instrument_id,
                                .symbol = symbol,
                                .bid_price =
                                    RequiredDecimal(item, "bp"),
                                .bid_size =
                                    RequiredDecimal(item, "bs"),
                                .bid_exchange = String(item, "bx"),
                                .ask_price =
                                    RequiredDecimal(item, "ap"),
                                .ask_size =
                                    RequiredDecimal(item, "as"),
                                .ask_exchange = String(item, "ax"),
                                .conditions = StringArray(item, "c"),
                                .tape = String(item, "z"),
                                .broker_timestamp = timestamp,
                                .event_time_ns = event_time_ns,
                                .received_at_ms = received_at_ms,
                            },
                        });
                const auto& quote =
                    std::get<tradebox::core::QuoteReceived>(
                        *event).quote;
                const std::string identity =
                    timestamp + "|" +
                    quote.bid_price.ToString() + "|" +
                    quote.ask_price.ToString() + "|" +
                    quote.bid_size.ToString() + "|" +
                    quote.ask_size.ToString();
                database_.QueueMarketDataEvent(
                    feed,
                    symbol + ":q:" +
                        std::to_string(event_time_ns) + ":" +
                        StablePayloadId(identity),
                    event);
                market_data_.Ingest(std::move(event));
            }
            if (snapshot.contains("latestTrade") &&
                snapshot["latestTrade"].is_object()) {
                const json& item = snapshot["latestTrade"];
                const std::string timestamp =
                    String(item, "t");
                const std::int64_t event_time_ns =
                    ParseTimestampNs(timestamp);
                const std::int64_t received_at_ms =
                    WallClockNowMs();
                const std::string trade_id =
                    Identifier(item, "i");
                auto event =
                    tradebox::core::ShareMarketDataEvent(
                        tradebox::core::TradeReceived{
                            .trade = {
                                .instrument_id = instrument_id,
                                .symbol = symbol,
                                .trade_id = trade_id,
                                .price =
                                    RequiredDecimal(item, "p"),
                                .size =
                                    RequiredDecimal(item, "s"),
                                .exchange = String(item, "x"),
                                .conditions = StringArray(item, "c"),
                                .tape = String(item, "z"),
                                .broker_timestamp = timestamp,
                                .event_time_ns = event_time_ns,
                                .received_at_ms = received_at_ms,
                            },
                        });
                database_.QueueMarketDataEvent(
                    feed,
                    symbol + ":" +
                        std::to_string(event_time_ns) + ":" +
                        trade_id,
                    event);
                market_data_.Ingest(std::move(event));
            }
        }
    } catch (const std::exception& error) {
        events_.Push({
            UiEventType::Status, {},
            "Market snapshot seed JSON error: " +
                std::string(error.what())});
    }
}

void AlpacaService::FetchAssetCatalog() {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    const std::wstring host = credentials.paper ? L"paper-api.alpaca.markets"
                                                : L"api.alpaca.markets";
    const HttpResult assets_response =
        Get(host, L"/v2/assets?status=active&asset_class=us_equity", credentials);
    if (assets_response.status != 200) {
        events_.Push({UiEventType::Status, {},
                      "Tradable asset catalog failed: " +
                          (!assets_response.error.empty() ? assets_response.error
                                                           : assets_response.body)});
        return;
    }
    try {
        const json value = json::parse(assets_response.body);
        if (!value.is_array()) {
            events_.Push({UiEventType::Status, {},
                          "Tradable asset catalog returned a non-array response"});
            return;
        }
        std::vector<tradebox::core::TradableAsset> assets;
        for (const auto& item : value) {
            tradebox::core::TradableAsset asset;
            asset.provider_asset_id = String(item, "id");
            if (!asset.provider_asset_id.empty())
                asset.instrument_id =
                    "alpaca:" + asset.provider_asset_id;
            asset.symbol = String(item, "symbol");
            asset.name = String(item, "name");
            asset.exchange = String(item, "exchange");
            asset.active = String(item, "status") == "active";
            asset.tradable = item.value("tradable", false);
            asset.shortable = item.value("shortable", false);
            asset.fractionable = item.value("fractionable", false);
            asset.received_at_ms = WallClockNowMs();
            if (!asset.symbol.empty()) assets.push_back(std::move(asset));
        }
        {
            std::unordered_map<std::string, std::string> identities;
            identities.reserve(assets.size());
            for (const auto& asset : assets) {
                if (!asset.symbol.empty() &&
                    !asset.instrument_id.empty())
                    identities.emplace(
                        asset.symbol, asset.instrument_id);
            }
            std::scoped_lock lock(asset_catalog_mutex_);
            instrument_ids_by_symbol_ = std::move(identities);
        }
        // The asset master is intentionally fetched separately from activity
        // enrichment. Sweeping snapshots for the entire universe here can
        // issue hundreds of network requests and make shutdown appear hung.
        UiEvent event;
        event.type = UiEventType::AssetCatalogReady;
        event.assets = std::move(assets);
        events_.Push(std::move(event));
    } catch (const std::exception& error) {
        events_.Push({UiEventType::Status, {},
                      "Tradable asset catalog JSON error: " +
                          std::string(error.what())});
    }
}

void AlpacaService::Disconnect() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    running_ = false;
    connected_ = false;
    account_connected_ = false;
    HINTERNET socket = websocket_.exchange(nullptr);
    if (socket) WinHttpCloseHandle(socket);
    HINTERNET account_socket = account_websocket_.exchange(nullptr);
    if (account_socket) WinHttpCloseHandle(account_socket);
    if (stream_thread_.joinable()) stream_thread_.join();
    if (account_stream_thread_.joinable()) account_stream_thread_.join();
    JoinWorkers();
    {
        std::scoped_lock lock(credentials_mutex_);
        if (!credentials_.key.empty())
            SecureZeroMemory(credentials_.key.data(),
                             credentials_.key.size());
        if (!credentials_.secret.empty())
            SecureZeroMemory(credentials_.secret.data(),
                             credentials_.secret.size());
        credentials_ = {};
    }
}

void AlpacaService::JoinWorkers() {
    std::vector<std::thread> workers;
    {
        std::scoped_lock lock(workers_mutex_);
        workers.swap(workers_);
    }
    for (std::thread& worker : workers)
        if (worker.joinable()) worker.join();
}

void AlpacaService::PublishCoreEvent(tradebox::core::BrokerEvent event) {
    if (event.generation.value == 0)
        event.generation =
            tradebox::core::ConnectionGeneration{active_generation_.load()};
    if (auto result = core_.Ingest(std::move(event)); !result) {
        events_.Push(
            {UiEventType::Status, {},
             "Trading core rejected broker event: " +
                 result.error().message});
    }
}

tradebox::broker::BrokerCommandResult AlpacaService::PlaceOrder(
    const tradebox::core::NativeOrderRequest& request) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    const auto body = tradebox::broker::alpaca::SerializeOrder(request);
    if (!body)
        return {
            .disposition =
                tradebox::broker::BrokerCommandDisposition::Rejected,
            .message = body.error(),
        };
    const AlpacaCredentials credentials = CredentialsSnapshot();
    if (credentials.key.empty())
        return {
            .disposition =
                tradebox::broker::BrokerCommandDisposition::Indeterminate,
            .message = "Broker credentials are unavailable",
        };
    const std::wstring host =
        credentials.paper ? L"paper-api.alpaca.markets"
                          : L"api.alpaca.markets";
    auto result = CommandResult(
        Request(L"POST", host, L"/v2/orders", credentials, *body));
    if (result.disposition ==
        tradebox::broker::BrokerCommandDisposition::Accepted)
        orders_dirty_ = true;
    return result;
}

tradebox::broker::BrokerCommandResult AlpacaService::CancelOrder(
    const std::string& order_id) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (order_id.empty())
        return {
            .disposition =
                tradebox::broker::BrokerCommandDisposition::Rejected,
            .message = "order_id is required",
        };
    const AlpacaCredentials credentials = CredentialsSnapshot();
    if (credentials.key.empty())
        return {
            .disposition =
                tradebox::broker::BrokerCommandDisposition::Indeterminate,
            .message = "Broker credentials are unavailable",
        };
    const std::wstring host =
        credentials.paper ? L"paper-api.alpaca.markets"
                          : L"api.alpaca.markets";
    auto result = CommandResult(Request(
        L"DELETE", host, Wide("/v2/orders/" + UrlEncode(order_id)),
        credentials));
    if (result.disposition ==
        tradebox::broker::BrokerCommandDisposition::Accepted)
        orders_dirty_ = true;
    return result;
}

tradebox::broker::BrokerCommandResult AlpacaService::ReplaceOrder(
    const std::string& order_id,
    const tradebox::core::ReplaceOrderRequest& request) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    const auto body =
        tradebox::broker::alpaca::SerializeReplacement(request);
    if (!body)
        return {
            .disposition =
                tradebox::broker::BrokerCommandDisposition::Rejected,
            .message = body.error(),
        };
    const AlpacaCredentials credentials = CredentialsSnapshot();
    if (credentials.key.empty())
        return {
            .disposition =
                tradebox::broker::BrokerCommandDisposition::Indeterminate,
            .message = "Broker credentials are unavailable",
        };
    const std::wstring host =
        credentials.paper ? L"paper-api.alpaca.markets"
                          : L"api.alpaca.markets";
    auto result = CommandResult(Request(
        L"PATCH", host, Wide("/v2/orders/" + UrlEncode(order_id)),
        credentials, *body));
    if (result.disposition ==
        tradebox::broker::BrokerCommandDisposition::Accepted)
        orders_dirty_ = true;
    return result;
}

void AlpacaService::FetchAccount() {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    const std::wstring host = credentials.paper ? L"paper-api.alpaca.markets"
                                                 : L"api.alpaca.markets";
    const HttpResult result = Get(host, L"/v2/account", credentials);
    if (result.status != 200) {
        events_.Push({UiEventType::Status, {}, "Account login failed: " +
                                                   (!result.error.empty()
                                                        ? result.error
                                                        : result.body)});
        return;
    }
    try {
        const json value = json::parse(result.body);
        tradebox::core::AccountState core_account;
        core_account.id = value.value("id", "");
        core_account.account_number = value.value("account_number", "");
        core_account.status = value.value("status", "");
        core_account.crypto_status = value.value("crypto_status", "");
        core_account.currency = value.value("currency", "");
        core_account.multiplier = value.value("multiplier", "");
        core_account.equity = RequiredDecimal(value, "equity");
        core_account.last_equity = RequiredDecimal(value, "last_equity");
        core_account.portfolio_value =
            RequiredDecimal(value, "portfolio_value");
        core_account.cash = RequiredDecimal(value, "cash");
        core_account.buying_power = RequiredDecimal(value, "buying_power");
        core_account.non_marginable_buying_power =
            RequiredDecimal(value, "non_marginable_buying_power");
        core_account.regt_buying_power =
            RequiredDecimal(value, "regt_buying_power");
        core_account.long_market_value =
            RequiredDecimal(value, "long_market_value");
        core_account.short_market_value =
            RequiredDecimal(value, "short_market_value");
        core_account.initial_margin =
            RequiredDecimal(value, "initial_margin");
        core_account.maintenance_margin =
            RequiredDecimal(value, "maintenance_margin");
        core_account.last_maintenance_margin =
            RequiredDecimal(value, "last_maintenance_margin");
        core_account.sma = RequiredDecimal(value, "sma");
        core_account.accrued_fees =
            RequiredDecimal(value, "accrued_fees");
        core_account.pending_transfer_in =
            RequiredDecimal(value, "pending_transfer_in");
        core_account.pending_transfer_out =
            RequiredDecimal(value, "pending_transfer_out");
        core_account.account_blocked =
            value.value("account_blocked", false);
        core_account.trade_suspended_by_user =
            value.value("trade_suspended_by_user", false);
        core_account.trading_blocked =
            value.value("trading_blocked", false);
        core_account.transfers_blocked =
            value.value("transfers_blocked", false);
        core_account.shorting_enabled =
            value.value("shorting_enabled", false);
        core_account.received_at_ms = WallClockNowMs();
        const std::int64_t account_received_at =
            core_account.received_at_ms;
        PublishCoreEvent(tradebox::core::BrokerEvent{
            .kind = tradebox::core::BrokerEventKind::AccountSnapshot,
            .source_event_id =
                "account:" + std::to_string(active_generation_.load()) +
                ":" + std::to_string(account_received_at),
            .raw_payload = result.body,
            .payload = tradebox::core::AccountSnapshotPayload{
                .account = std::move(core_account),
            },
        });
    } catch (const std::exception& error) {
        events_.Push(
            {UiEventType::Status, {}, "Account JSON error: " + std::string(error.what())});
    }
}

void AlpacaService::FetchPositions() {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    const std::wstring host = credentials.paper ? L"paper-api.alpaca.markets"
                                                 : L"api.alpaca.markets";
    const HttpResult result = Get(host, L"/v2/positions", credentials);
    if (result.status != 200) {
        events_.Push({UiEventType::Status, {}, "Positions refresh failed: " +
                                                   (!result.error.empty()
                                                        ? result.error
                                                        : result.body)});
        return;
    }
    try {
        const json value = json::parse(result.body);
        std::vector<tradebox::core::PositionState> positions;
        positions.reserve(value.size());
        for (const auto& item : value) {
            tradebox::core::PositionState position;
            position.asset_id = item.value("asset_id", "");
            position.symbol = item.value("symbol", "");
            position.exchange = item.value("exchange", "");
            position.asset_class = item.value("asset_class", "");
            position.side = item.value("side", "");
            position.qty = RequiredDecimal(item, "qty");
            position.qty_available =
                RequiredDecimal(item, "qty_available");
            position.avg_entry_price =
                RequiredDecimal(item, "avg_entry_price");
            position.market_value =
                RequiredDecimal(item, "market_value");
            position.cost_basis = RequiredDecimal(item, "cost_basis");
            position.unrealized_pl =
                RequiredDecimal(item, "unrealized_pl");
            position.unrealized_plpc =
                RequiredDecimal(item, "unrealized_plpc");
            position.unrealized_intraday_pl =
                RequiredDecimal(item, "unrealized_intraday_pl");
            position.unrealized_intraday_plpc =
                RequiredDecimal(item, "unrealized_intraday_plpc");
            position.current_price =
                RequiredDecimal(item, "current_price");
            position.lastday_price =
                RequiredDecimal(item, "lastday_price");
            position.change_today =
                RequiredDecimal(item, "change_today");
            positions.push_back(std::move(position));
        }
        const std::int64_t received_at = WallClockNowMs();
        PublishCoreEvent(tradebox::core::BrokerEvent{
            .kind = tradebox::core::BrokerEventKind::PositionsSnapshot,
            .source_event_id =
                "positions:" +
                std::to_string(active_generation_.load()) + ":" +
                std::to_string(received_at),
            .raw_payload = result.body,
            .payload = tradebox::core::PositionsSnapshotPayload{
                .positions = std::move(positions),
                .received_at_ms = received_at,
            },
        });
    } catch (const std::exception& error) {
        events_.Push({UiEventType::Status, {},
                      "Positions JSON error: " + std::string(error.what())});
    }
}

void AlpacaService::FetchOrders() {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    const std::wstring host = credentials.paper ? L"paper-api.alpaca.markets"
                                                 : L"api.alpaca.markets";
    std::vector<tradebox::core::OrderState> core_orders;
    json raw_orders = json::array();
    std::string until;
    while (running_) {
        std::string path =
            "/v2/orders?status=all&limit=500&direction=desc&nested=true";
        if (!until.empty()) path += "&until=" + UrlEncode(until);
        const HttpResult result = Get(host, Wide(path), credentials);
        if (result.status != 200) {
            events_.Push({UiEventType::Status, {}, "Orders load failed: " +
                                                       (!result.error.empty()
                                                            ? result.error
                                                            : result.body)});
            return;
        }
        try {
            const json value = json::parse(result.body);
            for (const auto& item : value) {
                core_orders.push_back(ParseCoreOrder(item));
                raw_orders.push_back(item);
            }
            if (value.size() < 500 || value.empty()) break;
            const std::string next_until =
                String(value.back(), "submitted_at");
            if (next_until.empty() || next_until == until) break;
            until = next_until;
        } catch (const std::exception& error) {
            events_.Push({UiEventType::Status, {},
                          "Orders JSON error: " + std::string(error.what())});
            return;
        }
    }
    const std::int64_t received_at = WallClockNowMs();
    PublishCoreEvent(tradebox::core::BrokerEvent{
        .kind = tradebox::core::BrokerEventKind::OrdersSnapshot,
        .source_event_id =
            "orders:" + std::to_string(active_generation_.load()) + ":" +
            std::to_string(received_at),
        .raw_payload = raw_orders.dump(),
        .payload = tradebox::core::OrdersSnapshotPayload{
            .orders = std::move(core_orders),
            .received_at_ms = received_at,
        },
    });
}

void AlpacaService::AccountRefreshLoop() {
    auto next_account_refresh = std::chrono::steady_clock::now();
    auto next_safety_reconciliation = std::chrono::steady_clock::now();
    auto next_order_safety_reconciliation = std::chrono::steady_clock::now();
    while (running_) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_account_refresh) {
            FetchAccount();
            next_account_refresh = now + std::chrono::seconds(5);
        }
        if (positions_dirty_.exchange(false) ||
            now >= next_safety_reconciliation) {
            FetchPositions();
            next_safety_reconciliation =
                now + std::chrono::seconds(1);
        }
        const bool stream_unavailable = !account_connected_.load();
        const bool order_safety_due =
            stream_unavailable && now >= next_order_safety_reconciliation;
        if ((orders_dirty_.exchange(false) || order_safety_due) && running_) {
            FetchOrders();
            next_order_safety_reconciliation =
                now + std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void AlpacaService::FetchMarketClock() {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    const std::wstring host = credentials.paper ? L"paper-api.alpaca.markets"
                                                 : L"api.alpaca.markets";
    const HttpResult result = Get(host, L"/v2/clock", credentials);
    if (result.status != 200) {
        events_.Push({UiEventType::Status, {}, "Market clock failed: " +
                                                   (!result.error.empty()
                                                        ? result.error
                                                        : result.body)});
        return;
    }
    try {
        const json value = json::parse(result.body);
        MarketClockSnapshot clock;
        clock.is_open = value.value("is_open", false);
        clock.timestamp_ms = ParseTimestampMs(value.value("timestamp", ""));
        clock.next_open_ms = ParseTimestampMs(value.value("next_open", ""));
        clock.next_close_ms = ParseTimestampMs(value.value("next_close", ""));
        clock.received_at_ms = WallClockNowMs();
        database_.QueueTimelineEvent(
            "alpaca.trading", std::to_string(clock.timestamp_ms),
            "market_clock", "", clock.timestamp_ms, result.body);
        UiEvent event;
        event.type = UiEventType::MarketClock;
        event.market_clock = clock;
        events_.Push(std::move(event));
    } catch (const std::exception& error) {
        events_.Push({UiEventType::Status, {},
                      "Market clock JSON error: " + std::string(error.what())});
    }
}

void AlpacaService::MarketClockLoop() {
    while (running_) {
        FetchMarketClock();
        for (int tenth_seconds = 0;
             tenth_seconds < 300 && running_; ++tenth_seconds)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void AlpacaService::FetchHistory(std::string symbol, std::string timeframe) {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    const tradebox::core::MarketDataFeed feed =
        market_data_feed_;
    const HttpResult result =
        Get(L"data.alpaca.markets",
            HistoryPath(symbol, timeframe, feed), credentials);
    if (result.status != 200) {
        events_.Push({UiEventType::Status, symbol,
                      "History failed for " + symbol + ": " +
                          (!result.error.empty() ? result.error : result.body)});
        return;
    }
    try {
        const json value = json::parse(result.body);
        std::vector<Bar> bars;
        std::vector<tradebox::core::MarketBar> core_bars;
        for (const auto& item : value.value("bars", json::array())) {
            const std::int64_t timestamp_ns =
                ParseTimestampNs(item.value("t", ""));
            bars.push_back({
                timestamp_ns / 1'000'000,
                Number(item, "o"),
                Number(item, "h"),
                Number(item, "l"),
                Number(item, "c"),
                Number(item, "v"),
            });
            core_bars.push_back({
                .start_ns = timestamp_ns,
                .open = RequiredDecimal(item, "o"),
                .high = RequiredDecimal(item, "h"),
                .low = RequiredDecimal(item, "l"),
                .close = RequiredDecimal(item, "c"),
                .volume = RequiredDecimal(item, "v"),
                .within_bar_vwap =
                    OptionalDecimal(item, "vw"),
                .trade_count = static_cast<std::uint64_t>(
                    std::max(0.0, Number(item, "n"))),
                .source = tradebox::core::BarSource::
                    ProviderHistorical,
                .state =
                    tradebox::core::BarState::Finalized,
            });
        }
        std::ranges::reverse(bars);
        std::ranges::reverse(core_bars);
        tradebox::core::BarUpsertBatch bar_batch{
            .key = {
                .instrument_id =
                    InstrumentIdForSymbol(symbol),
                .feed = feed,
                .timeframe = timeframe,
                .adjustment =
                    tradebox::core::BarAdjustment::All,
            },
            .symbol = symbol,
            .bars = std::move(core_bars),
        };
        database_.StoreProviderBars(bar_batch);
        bars_.Upsert(std::move(bar_batch));
        if (timeframe == "1Day") database_.StoreBars(symbol, bars);
        UiEvent event;
        event.type = UiEventType::HistoricalBars;
        event.symbol = std::move(symbol);
        event.timeframe = std::move(timeframe);
        event.bars = std::move(bars);
        events_.Push(std::move(event));
    } catch (const std::exception& error) {
        events_.Push({UiEventType::Status, symbol,
                      "History JSON error: " + std::string(error.what())});
    }
}

void AlpacaService::StreamLoop(std::vector<std::string> symbols) {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    const tradebox::core::MarketDataFeed feed = market_data_feed_;
    const bool sip = feed == tradebox::core::MarketDataFeed::Sip;
    const char* feed_name = sip ? "SIP" : "IEX";
    market_data_.Ingest(tradebox::core::MarketStreamChanged{
        .status = tradebox::core::MarketStreamStatus::Connecting,
        .feed = feed,
        .message = std::string("Connecting to ") + feed_name +
                   " market stream",
        .received_at_ms = WallClockNowMs(),
    });
    HINTERNET session =
        WinHttpOpen(L"TradeBoxNative/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 0);
    HINTERNET connection =
        WinHttpConnect(session, L"stream.data.alpaca.markets",
                       INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request =
        connection
            ? WinHttpOpenRequest(connection, L"GET",
                                 sip ? L"/v2/sip" : L"/v2/iex", nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE)
            : nullptr;
    if (!request ||
        !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        events_.Push({UiEventType::Status, {}, "Market stream connection failed"});
        market_data_.Ingest(tradebox::core::MarketStreamChanged{
            .status = tradebox::core::MarketStreamStatus::Error,
            .feed = feed,
            .message = "Market stream connection failed",
            .received_at_ms = WallClockNowMs(),
        });
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return;
    }
    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    if (!socket) {
        events_.Push({UiEventType::Status, {}, "WebSocket upgrade failed"});
        market_data_.Ingest(tradebox::core::MarketStreamChanged{
            .status = tradebox::core::MarketStreamStatus::Error,
            .feed = feed,
            .message = "Market WebSocket upgrade failed",
            .received_at_ms = WallClockNowMs(),
        });
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return;
    }
    websocket_ = socket;
    const json auth = {
        {"action", "auth"},
        {"key", credentials.key},
        {"secret", credentials.secret},
    };
    {
        std::scoped_lock send_lock(websocket_send_mutex_);
        SendText(socket, auth.dump());
    }

    std::string message;
    std::vector<char> buffer(64 * 1024);
    while (running_ && websocket_.load() == socket) {
        DWORD bytes = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD rc = WinHttpWebSocketReceive(
            socket, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, &type);
        if (rc != NO_ERROR) break;
        message.append(buffer.data(), bytes);
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
            continue;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        try {
            auto frame = tradebox::broker::alpaca::DecodeMarketFrame(
                message, WallClockNowMs(),
                [this](std::string_view symbol) {
                    return InstrumentIdForSymbol(
                        std::string(symbol));
                });
            for (auto& control : frame.controls) {
                using tradebox::broker::alpaca::StreamControlType;
                if (control.type ==
                    StreamControlType::Authenticated) {
                    connected_ = true;
                    market_data_.Ingest(
                        tradebox::core::MarketStreamChanged{
                            .status = tradebox::core::
                                MarketStreamStatus::Authenticated,
                            .feed = feed,
                            .message = std::string(feed_name) +
                                       " market stream authenticated",
                            .received_at_ms = WallClockNowMs(),
                        });
                    std::vector<std::string> desired;
                    {
                        std::scoped_lock subscription_lock(
                            subscription_mutex_);
                        desired = desired_symbols_;
                        subscribed_symbols_ = desired;
                    }
                    if (!desired.empty()) {
                        std::scoped_lock send_lock(
                            websocket_send_mutex_);
                        SendText(
                            socket,
                            Subscription("subscribe", desired));
                    }
                    events_.Push({
                        UiEventType::Status, {},
                        "Market stream authenticated",
                    });
                } else if (control.type ==
                           StreamControlType::Error) {
                    const std::string error_message =
                        "Stream error: " + control.message;
                    events_.Push({
                        UiEventType::Status, {}, error_message,
                    });
                    market_data_.Ingest(
                        tradebox::core::MarketStreamChanged{
                            .status = tradebox::core::
                                MarketStreamStatus::Error,
                            .feed = feed,
                            .message = error_message,
                            .received_at_ms = WallClockNowMs(),
                        });
                } else {
                    const std::size_t subscribed =
                        control.trade_symbols.size();
                    events_.Push({
                        UiEventType::Status,
                        {},
                        "Market subscription active: " +
                            std::to_string(subscribed) +
                            " symbols",
                    });
                    market_data_.Ingest(
                        tradebox::core::MarketStreamChanged{
                            .status = tradebox::core::
                                MarketStreamStatus::Subscribed,
                            .feed = feed,
                            .trade_symbols =
                                control.trade_symbols,
                            .quote_symbols =
                                control.quote_symbols,
                            .message = std::string(feed_name) +
                                       " trades and quotes subscribed",
                            .received_at_ms = WallClockNowMs(),
                        });
                    if (!control.trade_symbols.empty()) {
                        std::scoped_lock worker_lock(
                            workers_mutex_);
                        workers_.emplace_back(
                            &AlpacaService::SeedLatestSnapshots,
                            this, control.trade_symbols);
                    }
                }
            }
            for (auto& decoded : frame.items) {
                if (decoded.market_tick) {
                    database_.QueueMarketDataEvent(
                        sip ? "sip" : "iex",
                        std::move(decoded.source_event_id),
                        decoded.market_event);
                }
                if (decoded.market_event)
                    market_data_.Ingest(
                        std::move(decoded.market_event));
                if (decoded.bar) {
                    const auto& bar = *decoded.bar;
                    tradebox::core::BarUpsertBatch bar_batch{
                        .key = {
                            .instrument_id =
                                bar.instrument_id,
                            .feed = feed,
                            .timeframe =
                                bar.kind == "d"
                                    ? "1Day"
                                    : "1Min",
                            .adjustment =
                                tradebox::core::
                                    BarAdjustment::Raw,
                        },
                        .symbol = bar.symbol,
                        .bars = {{
                            .start_ns =
                                bar.timestamp_ms *
                                1'000'000,
                            .open = bar.open,
                            .high = bar.high,
                            .low = bar.low,
                            .close = bar.close,
                            .volume = bar.volume,
                            .within_bar_vwap =
                                bar.within_bar_vwap,
                            .trade_count = bar.trade_count,
                            .source =
                                tradebox::core::BarSource::
                                    ProviderStream,
                            .state =
                                bar.kind == "d"
                                    ? tradebox::core::
                                          BarState::Open
                                    : tradebox::core::
                                          BarState::Finalized,
                        }},
                    };
                    bars_.Upsert(bar_batch);
                    if (!database_.QueueProviderBars(
                            std::move(bar_batch))) {
                        events_.Push({
                            UiEventType::Status,
                            bar.symbol,
                            "Provider-bar persistence queue full; "
                            "live bar remains available in memory",
                        });
                    }
                    if (bar.kind != "d") continue;
                    UiEvent event;
                    event.type = UiEventType::DailyBar;
                    event.symbol = bar.symbol;
                    event.bar = {
                        bar.timestamp_ms,
                        bar.open.ToDisplayDouble(),
                        bar.high.ToDisplayDouble(),
                        bar.low.ToDisplayDouble(),
                        bar.close.ToDisplayDouble(),
                        bar.volume.ToDisplayDouble(),
                    };
                    event.received_at_ms =
                        decoded.received_at_ms;
                    events_.Push(std::move(event));
                }
            }
        } catch (const std::exception& error) {
            events_.Push({UiEventType::Status, {},
                          "Stream JSON error: " + std::string(error.what())});
        }
        message.clear();
    }
    connected_ = false;
    if (websocket_.exchange(nullptr) == socket) WinHttpCloseHandle(socket);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (running_)
        events_.Push({UiEventType::Status, {}, "Market stream disconnected"});
    market_data_.Ingest(tradebox::core::MarketStreamChanged{
        .status = running_ ? tradebox::core::MarketStreamStatus::Stale
                           : tradebox::core::MarketStreamStatus::Disconnected,
        .feed = feed,
        .message =
            running_
                ? std::string(feed_name) +
                      " market stream disconnected unexpectedly"
                : std::string(feed_name) + " market stream stopped",
        .received_at_ms = WallClockNowMs(),
    });
}

void AlpacaService::AccountStreamLoop() {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    HINTERNET session =
        WinHttpOpen(L"TradeBoxNative/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    const std::wstring host = credentials.paper ? L"paper-api.alpaca.markets"
                                                 : L"api.alpaca.markets";
    HINTERNET connection =
        session ? WinHttpConnect(session, host.c_str(),
                                 INTERNET_DEFAULT_HTTPS_PORT, 0)
                : nullptr;
    HINTERNET request =
        connection
            ? WinHttpOpenRequest(connection, L"GET", L"/stream", nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE)
            : nullptr;
    if (!session || !request ||
        !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr,
                          0) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        events_.Push(
            {UiEventType::Status, {}, "Account stream connection failed"});
        PublishCoreEvent(tradebox::core::BrokerEvent{
            .kind = tradebox::core::BrokerEventKind::Failure,
            .raw_payload = "{}",
            .message = "Account stream connection failed",
        });
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
        return;
    }

    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    if (!socket) {
        events_.Push(
            {UiEventType::Status, {}, "Account WebSocket upgrade failed"});
        PublishCoreEvent(tradebox::core::BrokerEvent{
            .kind = tradebox::core::BrokerEventKind::Failure,
            .raw_payload = "{}",
            .message = "Account WebSocket upgrade failed",
        });
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return;
    }
    account_websocket_ = socket;
    const json auth = {
        {"action", "auth"},
        {"key", credentials.key},
        {"secret", credentials.secret},
    };
    const auto auth_started = std::chrono::steady_clock::now();
    SendText(socket, auth.dump());

    std::string message;
    std::vector<char> buffer(64 * 1024);
    while (running_ && account_websocket_.load() == socket) {
        DWORD bytes = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD rc = WinHttpWebSocketReceive(
            socket, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes,
            &type);
        if (rc != NO_ERROR) break;
        message.append(buffer.data(), bytes);
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
            continue;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        try {
            const json packet = json::parse(message);
            std::vector<const json*> items;
            if (packet.is_array()) {
                items.reserve(packet.size());
                for (const auto& item : packet)
                    items.push_back(&item);
            } else {
                items.push_back(&packet);
            }
            for (const json* item_pointer : items) {
                const json& item = *item_pointer;
                const std::string stream = item.value("stream", "");
                if (!item.contains("data") ||
                    !item["data"].is_object())
                    continue;
                const json& data = item["data"];
                if (stream == "authorization") {
                    const std::string status = data.value("status", "");
                    if (status == "authorized") {
                        PublishCoreEvent(tradebox::core::BrokerEvent{
                            .kind =
                                tradebox::core::BrokerEventKind::Authorized,
                            .source_event_id =
                                "authorized:" +
                                std::to_string(active_generation_.load()),
                            .raw_payload = item.dump(),
                        });
                        const json listen = {
                            {"action", "listen"},
                            {"data", {{"streams", {"trade_updates"}}}},
                        };
                        SendText(socket, listen.dump());
                    } else {
                        events_.Push({UiEventType::Status, {},
                                      "Account stream authorization failed"});
                        PublishCoreEvent(tradebox::core::BrokerEvent{
                            .kind = tradebox::core::BrokerEventKind::Failure,
                            .raw_payload = item.dump(),
                            .message =
                                "Account stream authorization failed",
                        });
                    }
                } else if (stream == "listening") {
                    bool trade_updates_active = false;
                    for (const auto& subscribed :
                         data.value("streams", json::array())) {
                        if (subscribed.is_string() &&
                            subscribed.get<std::string>() == "trade_updates") {
                            trade_updates_active = true;
                            break;
                        }
                    }
                    if (trade_updates_active && !account_connected_.exchange(true)) {
                        PublishCoreEvent(tradebox::core::BrokerEvent{
                            .kind = tradebox::core::BrokerEventKind::
                                TradeUpdatesAcknowledged,
                            .source_event_id =
                                "listening:" +
                                std::to_string(active_generation_.load()),
                            .raw_payload = item.dump(),
                        });
                        UiEvent event;
                        event.type = UiEventType::AccountStreamConnected;
                        event.latency_ms =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - auth_started)
                                .count();
                        event.received_at_ms = WallClockNowMs();
                        events_.Push(std::move(event));
                    } else if (!trade_updates_active) {
                        events_.Push({UiEventType::Status, {},
                                      "Account trade_updates subscription missing"});
                    }
                } else if (stream == "trade_updates") {
                    orders_dirty_ = true;
                    positions_dirty_ = true;
                    const std::int64_t received_at = WallClockNowMs();
                    const std::int64_t timestamp =
                        ParseTimestampMs(data.value("timestamp", ""));
                    database_.QueueTimelineEvent(
                        "alpaca.trading", "", "trade_update",
                        data.contains("order")
                            ? data["order"].value("symbol", "")
                            : "",
                        timestamp > 0 ? timestamp : received_at, item.dump());
                    if (!data.contains("order") ||
                        !data["order"].is_object()) {
                        throw std::runtime_error(
                            "trade_updates payload has no order object");
                    }
                    tradebox::core::TradeUpdatePayload update;
                    update.event = data.value("event", "");
                    update.execution_id =
                        data.value("execution_id", "");
                    update.order = ParseCoreOrder(data["order"]);
                    update.fill_qty = OptionalDecimal(data, "qty");
                    update.fill_price = OptionalDecimal(data, "price");
                    update.position_qty =
                        OptionalDecimal(data, "position_qty");
                    update.event_at_ms =
                        timestamp > 0 ? timestamp : received_at;
                    std::string source_event_id = update.execution_id;
                    if (source_event_id.empty()) {
                        source_event_id =
                            update.order.id + ":" + update.event + ":" +
                            std::to_string(update.event_at_ms);
                    }
                    PublishCoreEvent(tradebox::core::BrokerEvent{
                        .kind =
                            tradebox::core::BrokerEventKind::TradeUpdate,
                        .source_event_id = std::move(source_event_id),
                        .raw_payload = item.dump(),
                        .payload = std::move(update),
                    });
                    UiEvent event;
                    event.type = UiEventType::AccountStreamEvent;
                    event.received_at_ms = received_at;
                    event.latency_ms =
                        timestamp > 0 ? received_at - timestamp : -1;
                    events_.Push(std::move(event));
                }
            }
        } catch (const std::exception& error) {
            events_.Push({UiEventType::Status, {},
                          "Account stream JSON error: " +
                              std::string(error.what())});
        }
        message.clear();
    }

    account_connected_ = false;
    if (account_websocket_.exchange(nullptr) == socket)
        WinHttpCloseHandle(socket);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (running_)
    {
        events_.Push(
            {UiEventType::Status, {}, "Account stream disconnected"});
        PublishCoreEvent(tradebox::core::BrokerEvent{
            .kind = tradebox::core::BrokerEventKind::Disconnected,
            .raw_payload = "{}",
            .message = "Account stream disconnected",
        });
    }
}
