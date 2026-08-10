#include "tradebox/broker/alpaca_service.h"
#include "tradebox/broker/alpaca_auth.h"
#include "tradebox/broker/alpaca_page_validation.h"
#include "tradebox/broker/alpaca_account_activity.h"
#include "tradebox/broker/alpaca_market_stream_decoder.h"
#include "tradebox/broker/alpaca_order_codec.h"
#include "tradebox/broker/alpaca_payload_limits.h"
#include "tradebox/broker/alpaca_polling_policy.h"
#include "tradebox/broker/alpaca_rest_transport.h"
#include "tradebox/broker/alpaca_stream_supervision.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;

namespace {

using HttpResult = tradebox::broker::alpaca::RestResponse;

UiEvent OperationalEvent(
    OperationalComponent component,
    OperationalState state,
    OperationalSeverity severity,
    std::string message,
    OperationalReason reason = OperationalReason::None) {
    UiEvent event;
    event.type = UiEventType::Status;
    event.message = std::move(message);
    event.operational_component = component;
    event.operational_state = state;
    event.operational_reason = reason;
    event.operational_severity = severity;
    return event;
}

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

std::string Narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr,
        0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
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

HttpResult Request(
                   tradebox::broker::alpaca::AlpacaRestTransport& transport,
                   const wchar_t* method, const std::wstring& host,
                   const std::wstring& path,
                   const AlpacaCredentials& credentials,
                   std::string_view body = {},
                   tradebox::broker::alpaca::RestPriority priority =
                       tradebox::broker::alpaca::RestPriority::Background) {
    const std::wstring_view method_view{method};
    const std::string method_text =
        method_view == L"GET" ? "GET"
        : method_view == L"POST" ? "POST"
        : method_view == L"PATCH" ? "PATCH"
                                 : "DELETE";
    return transport.Execute({
        .method = method_text,
        .host = Narrow(host),
        .path = Narrow(path),
        .api_key = credentials.key,
        .api_secret = credentials.secret,
        .body = std::string(body),
        .domain =
            host == L"data.alpaca.markets"
                ? tradebox::broker::alpaca::RestDomain::MarketData
                : tradebox::broker::alpaca::RestDomain::Trading,
        .priority = priority,
        .safe_to_retry = method_text == "GET",
        .coalesce = method_text == "GET",
    });
}

HttpResult Get(
    tradebox::broker::alpaca::AlpacaRestTransport& transport,
    const std::wstring& host, const std::wstring& path,
    const AlpacaCredentials& credentials,
    tradebox::broker::alpaca::RestPriority priority =
        tradebox::broker::alpaca::RestPriority::Background) {
    return Request(
        transport, L"GET", host, path, credentials, {}, priority);
}

const char* AlpacaFeedName(
    tradebox::core::MarketDataFeed feed) {
    return feed == tradebox::core::MarketDataFeed::Sip
               ? "sip"
               : "iex";
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

tradebox::core::OrderState ParseCoreOrder(
    const json& value, std::string parent_order_id = {}) {
    tradebox::core::OrderState order;
    order.id = String(value, "id");
    order.parent_order_id = String(value, "parent_order_id");
    if (order.parent_order_id.empty())
        order.parent_order_id = std::move(parent_order_id);
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

// `nested=true` returns advanced orders as roots with recursive `legs`.
// Flatten them for the core while retaining their broker relationship.
void ParseCoreOrderTree(const json& value,
                        std::vector<tradebox::core::OrderState>& orders,
                        const std::string& parent_order_id = {}) {
    tradebox::core::OrderState order =
        ParseCoreOrder(value, parent_order_id);
    const std::string order_id = order.id;
    orders.push_back(std::move(order));
    if (!value.contains("legs") || !value["legs"].is_array()) return;
    for (const json& leg : value["legs"])
        if (leg.is_object()) ParseCoreOrderTree(leg, orders, order_id);
}

bool SendText(HINTERNET socket, const std::string& message,
              DWORD* error = nullptr) {
    const DWORD result = WinHttpWebSocketSend(
        socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(message.data()),
        static_cast<DWORD>(message.size()));
    if (error) *error = result;
    return result == NO_ERROR;
}

void ClearSensitiveString(std::string& value) noexcept {
    if (!value.empty())
        SecureZeroMemory(value.data(), value.size());
    value.clear();
}

bool ApplySecureRequestPolicy(HINTERNET request, DWORD& error) {
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(
            request, WINHTTP_OPTION_REDIRECT_POLICY,
            &redirect_policy, sizeof(redirect_policy))) {
        error = GetLastError();
        return false;
    }
    DWORD enabled_features = WINHTTP_ENABLE_SSL_REVOCATION;
    if (!WinHttpSetOption(
            request, WINHTTP_OPTION_ENABLE_FEATURE,
            &enabled_features, sizeof(enabled_features))) {
        error = GetLastError();
        return false;
    }
    return true;
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
        {"statuses", std::vector<std::string>{"*"}},
    };
    return value.dump();
}

std::string TickRequestKey(
    const tradebox::core::TickQuery& query) {
    return query.instrument_id + "\x1f" + query.symbol +
           "\x1f" + std::to_string(query.start_ns) +
           "\x1f" + std::to_string(query.end_ns) +
           "\x1f" +
           std::to_string(static_cast<int>(query.feed)) +
           "\x1f" + (query.include_trades ? "1" : "0") +
           (query.include_quotes ? "1" : "0");
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

tradebox::broker::BrokerCommandResult BulkCommandResult(
    const HttpResult& response) {
    if (response.status != 207) {
        auto result = CommandResult(response);
        result.reconciliation_required =
            result.disposition !=
            tradebox::broker::BrokerCommandDisposition::Rejected;
        return result;
    }

    tradebox::broker::BrokerCommandResult result{
        .http_status = response.status,
        .raw_response = response.body,
        .reconciliation_required = true,
    };
    try {
        const json value = json::parse(response.body);
        if (!value.is_array()) throw std::runtime_error("not an array");
        std::size_t accepted = 0;
        for (const auto& entry : value) {
            tradebox::core::CommandItemResult item;
            item.id = entry.value("id", "");
            item.http_status = entry.value("status", 0U);
            item.accepted =
                item.http_status >= 200 && item.http_status < 300;
            if (item.accepted) ++accepted;
            if (entry.contains("body")) {
                const auto& body = entry["body"];
                item.raw_response = body.dump();
                if (body.is_object()) {
                    if (item.id.empty()) item.id = body.value("id", "");
                    item.symbol = body.value("symbol", "");
                    item.message = body.value("message", "");
                } else if (body.is_string()) {
                    item.message = body.get<std::string>();
                }
            } else {
                item.raw_response = entry.dump();
            }
            if (item.message.empty())
                item.message =
                    item.accepted ? "Broker accepted item"
                                  : "Broker rejected item";
            result.items.push_back(std::move(item));
        }
        if (accepted == result.items.size()) {
            result.disposition =
                tradebox::broker::BrokerCommandDisposition::Accepted;
            result.message = "Broker accepted all bulk command items";
        } else if (accepted != 0) {
            result.disposition =
                tradebox::broker::BrokerCommandDisposition::PartiallyAccepted;
            result.message =
                "Broker accepted only part of the bulk command";
        } else {
            result.disposition =
                tradebox::broker::BrokerCommandDisposition::Rejected;
            result.message = "Broker rejected all bulk command items";
        }
    } catch (const std::exception&) {
        result.disposition =
            tradebox::broker::BrokerCommandDisposition::Indeterminate;
        result.message =
            "Broker returned multi-status with an unreadable response";
    }
    return result;
}

}  // namespace

AlpacaService::AlpacaService(UiEventQueue& events, Database& database,
                             tradebox::core::ITradingCore& core,
                             tradebox::core::IMarketDataSink& market_data,
                             tradebox::core::IMarketDataView& market_data_view,
                             tradebox::core::IBarDataSink& bars,
                             tradebox::broker::IHistoryRequestStatusSink*
                                 history_status)
    : events_(events),
      database_(database),
      core_(core),
      market_data_(market_data),
      market_data_view_(market_data_view),
      bars_(bars),
      history_status_(history_status) {
    for (std::size_t index = 0; index < 3; ++index)
        background_workers_.emplace_back(
            &AlpacaService::BackgroundWorkerLoop, this);
    for (const auto& asset : database_.LoadAssetCatalog()) {
        if (!asset.symbol.empty() && !asset.instrument_id.empty())
            instrument_ids_by_symbol_.insert_or_assign(
                asset.symbol, asset.instrument_id);
    }
}

AlpacaService::~AlpacaService() {
    Disconnect();
    {
        std::scoped_lock lock(background_mutex_);
        background_stopping_ = true;
    }
    background_ready_.notify_all();
    for (auto& worker : background_workers_)
        if (worker.joinable()) worker.join();
}

tradebox::core::RestTransportHealth AlpacaService::RestHealth() const {
    auto health = rest_transport_.Health();
    std::scoped_lock lock(background_mutex_);
    health.background_queued = background_tasks_.size();
    health.background_in_flight = background_active_;
    health.background_rejected = background_rejected_;
    health.background_coalesced =
        tick_requests_coalesced_.load();
    return health;
}

void AlpacaService::ReportPersistenceHealth() {
    const DatabaseWriterTelemetry health = database_.WriterTelemetry();
    if (health.write_failures <= reported_persistence_failures_) return;
    reported_persistence_failures_ = health.write_failures;
    events_.Push(OperationalEvent(
        OperationalComponent::Persistence,
        OperationalState::Failed,
        OperationalSeverity::Critical,
        "Market-data persistence failed: " +
            (health.last_write_error.empty()
                 ? std::string("unknown database error")
                 : health.last_write_error),
        OperationalReason::PersistenceFailure));
}

AlpacaCredentials AlpacaService::CredentialsSnapshot() const {
    std::scoped_lock lock(credentials_mutex_);
    return {
        credentials_.key,
        credentials_.secret,
        credentials_.paper,
    };
}

std::string AlpacaService::InstrumentIdForSymbol(
    const std::string& symbol) const {
    std::scoped_lock lock(asset_catalog_mutex_);
    const auto found = instrument_ids_by_symbol_.find(symbol);
    return found == instrument_ids_by_symbol_.end()
               ? std::string{}
               : found->second;
}

tradebox::core::BarSeriesKey AlpacaService::ResolveBarSeriesKey(
    const std::string& symbol, const std::string& timeframe) const {
    return {
        .instrument_id = InstrumentIdForSymbol(symbol),
        .feed = market_data_feed_,
        .timeframe = timeframe,
        .adjustment = tradebox::core::BarAdjustment::All,
    };
}

std::expected<tradebox::core::MarketDataFeed, std::string>
AlpacaService::ResolveMarketDataFeed(
    const AlpacaCredentials& credentials,
    tradebox::core::MarketDataFeed requested,
    const std::vector<std::string>& symbols) {
    if (requested != tradebox::core::MarketDataFeed::Unknown)
        return requested;

    // Probe the provider's broadest stock feed through the same authenticated
    // market-data endpoint used for snapshot seeding. A successful SIP
    // response proves that this connection can use SIP; otherwise IEX is the
    // best available feed for the current Alpaca account.
    const std::string probe_symbol =
        symbols.empty() ? std::string("AAPL") : symbols.front();
    rest_transport_.Resume();
    const auto response = Get(
        rest_transport_, L"data.alpaca.markets",
        Wide("/v2/stocks/trades/latest?symbols=" +
             UrlEncode(probe_symbol) + "&feed=sip"),
        credentials, tradebox::broker::alpaca::RestPriority::Interactive);
    if (response.status == 200) return tradebox::core::MarketDataFeed::Sip;
    return tradebox::core::MarketDataFeed::Iex;
}

void AlpacaService::Connect(AlpacaCredentials credentials,
                            const std::vector<std::string>& symbols,
                            tradebox::core::MarketDataFeed feed) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    Disconnect();
    rest_transport_.Resume();
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
    persistence_queue_warning_emitted_ = false;
    market_gap_started_ns_ = 0;
    {
        std::scoped_lock lock(workers_mutex_);
        orders_dirty_ = true;
        positions_dirty_ = true;
        activities_dirty_ = true;
        activities_identity_deferred_reported_ = false;
        account_dirty_ = true;
        workers_.emplace_back(&AlpacaService::AccountRefreshLoop, this);
        workers_.emplace_back(&AlpacaService::MarketClockLoop, this);
    }
    stream_thread_ = std::thread(&AlpacaService::StreamLoop, this);
    account_stream_thread_ =
        std::thread(&AlpacaService::AccountStreamLoop, this);
}

void AlpacaService::RequestAccountActivities() {
    if (running_) activities_dirty_ = true;
}

void AlpacaService::RefreshSymbols(const std::vector<std::string>& symbols) {
    if (!running_) return;
    std::vector<std::string> previous;
    {
        std::scoped_lock lock(subscription_mutex_);
        desired_symbols_ = symbols;
        previous = subscribed_symbols_;
    }
    HINTERNET socket = websocket_.load();
    if (!socket || !connected_) return;
    std::scoped_lock send_lock(websocket_send_mutex_);
    if (!previous.empty())
        SendText(socket, Subscription("unsubscribe", previous));
    if (!symbols.empty())
        SendText(socket, Subscription("subscribe", symbols));
    if (!symbols.empty()) {
        if (!SubmitBackground(
                [this, symbols] { SeedLatestSnapshots(symbols); }))
            events_.Push({UiEventType::Status, {},
                          "Snapshot seed rejected: background queue full"});
    }
}

void AlpacaService::RequestHistory(const std::string& symbol,
                                   const std::string& timeframe) {
    const auto now = std::chrono::system_clock::now();
    const auto start =
        now - std::chrono::hours(24 * 365 * 6);
    const auto nanoseconds = [](auto point) {
        return std::chrono::duration_cast<
                   std::chrono::nanoseconds>(
                   point.time_since_epoch())
            .count();
    };
    RequestHistory(
        symbol, timeframe,
        {nanoseconds(start), nanoseconds(now)});
}

void AlpacaService::RequestHistory(
    const std::string& symbol,
    const std::string& timeframe,
    tradebox::core::BarRange range) {
    RequestHistory({
        .key = {
            .instrument_id = InstrumentIdForSymbol(symbol),
            .feed = market_data_feed_,
            .timeframe = timeframe,
            .adjustment =
                tradebox::core::BarAdjustment::All,
        },
        .symbol = symbol,
        .range = range,
    });
}

void AlpacaService::RequestHistory(
    tradebox::core::HistoricalBarQuery query) {
    if (!running_) {
        if (history_status_)
            history_status_->Publish({
                .key = query.key,
                .range = query.range,
                .state = tradebox::broker::HistoryRequestState::Failed,
                .message = "History is unavailable while disconnected",
            });
        return;
    }
    const std::string request_symbol = query.symbol;
    const tradebox::core::BarSeriesKey request_key = query.key;
    const tradebox::core::BarRange request_range = query.range;
    if (!SubmitBackground(
        [this, query = std::move(query)] {
            const auto& key = query.key;
            const auto& symbol = query.symbol;
            const auto range = query.range;
            if (key.instrument_id.empty() ||
                symbol.empty() || key.timeframe.empty() ||
                key.feed ==
                    tradebox::core::MarketDataFeed::Unknown ||
                range.start_ns < 0 ||
                range.start_ns >= range.end_ns) {
                events_.Push({
                    UiEventType::Status,
                    symbol,
                    "History request has no resolved instrument "
                    "identity or valid range",
                });
                if (history_status_)
                    history_status_->Publish({
                        .key = key,
                        .range = range,
                        .state = tradebox::broker::HistoryRequestState::Failed,
                        .message =
                            "History request has no resolved instrument identity or valid range",
                    });
                return;
            }
            const StoredBarSeries stored =
                database_.LoadProviderBars(key, range);
            const auto missing =
                tradebox::core::MissingBarRanges(
                    stored.coverage, range);
            auto reserved =
                in_flight_bar_ranges_.Reserve(key, missing);
            if (reserved.empty()) {
                PublishCachedHistory(symbol, key, range);
                if (history_status_)
                    history_status_->Publish({
                        .key = key,
                        .range = range,
                        .state = missing.empty()
                                     ? tradebox::broker::HistoryRequestState::Succeeded
                                     : tradebox::broker::HistoryRequestState::Loading,
                    });
                return;
            }
            if (history_status_)
                history_status_->Publish({
                    .key = key,
                    .range = range,
                    .state = tradebox::broker::HistoryRequestState::Loading,
                });
            FetchHistory(symbol, key, range,
                         std::move(reserved));
        })) {
        events_.Push({UiEventType::Status, request_symbol,
                      "History request rejected: background queue full"});
        if (history_status_)
            history_status_->Publish({
                .key = request_key,
                .range = request_range,
                .state = tradebox::broker::HistoryRequestState::Failed,
                .message =
                    "History request rejected: background queue full",
            });
    }
}

void AlpacaService::RequestAssetCatalog() {
    if (!running_) return;
    if (!SubmitBackground([this] { FetchAssetCatalog(); }))
        events_.Push({UiEventType::Status, {},
                      "Asset catalog rejected: background queue full"});
}

std::future<tradebox::core::TickSeries> AlpacaService::RequestTicks(
    tradebox::core::TickQuery query) {
    auto promise =
        std::make_shared<std::promise<tradebox::core::TickSeries>>();
    std::future<tradebox::core::TickSeries> result =
        promise->get_future();
    const std::string request_key = TickRequestKey(query);
    const tradebox::core::TickQuery rejected_query = query;
    {
        std::scoped_lock lock(tick_requests_mutex_);
        const auto found = tick_requests_.find(request_key);
        if (found != tick_requests_.end()) {
            found->second.push_back(promise);
            ++tick_requests_coalesced_;
            return result;
        }
        tick_requests_.emplace(
            request_key,
            std::vector{promise});
    }
    if (!SubmitBackground(
            [this, request_key,
             query = std::move(query)]() {
                tradebox::core::TickSeries completed{
                    .query = query};
                try {
                    completed = FetchTicks(query);
                } catch (const std::exception& error) {
                    completed.error = error.what();
                }
                std::vector<std::shared_ptr<std::promise<
                    tradebox::core::TickSeries>>>
                    waiters;
                {
                    std::scoped_lock lock(
                        tick_requests_mutex_);
                    const auto found =
                        tick_requests_.find(request_key);
                    if (found != tick_requests_.end()) {
                        waiters =
                            std::move(found->second);
                        tick_requests_.erase(found);
                    }
                }
                for (const auto& waiter : waiters)
                    waiter->set_value(completed);
            })) {
        tradebox::core::TickSeries failed{.query = rejected_query};
        failed.error = "Tick request rejected: background queue full";
        std::vector<std::shared_ptr<std::promise<
            tradebox::core::TickSeries>>>
            waiters;
        {
            std::scoped_lock lock(tick_requests_mutex_);
            const auto found =
                tick_requests_.find(request_key);
            if (found != tick_requests_.end()) {
                waiters = std::move(found->second);
                tick_requests_.erase(found);
            }
        }
        for (const auto& waiter : waiters)
            waiter->set_value(failed);
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
    const std::string instrument_id =
        query.instrument_id.empty()
            ? InstrumentIdForSymbol(query.symbol)
            : query.instrument_id;
    if (instrument_id.empty()) {
        result.error =
            "Tick request has no resolved stable instrument identity";
        return result;
    }
    tradebox::core::TickQuery resolved_query = query;
    resolved_query.instrument_id = instrument_id;
    result.query = resolved_query;
    if (credentials.key.empty() || credentials.secret.empty()) {
        result = database_.LoadMarketTicks(resolved_query);
        result.missing_trade_ranges =
            query.include_trades
                ? database_.MissingMarketTickCoverage(
                      resolved_query, "t")
                : std::vector<tradebox::core::TickCoverage>{};
        result.missing_quote_ranges =
            query.include_quotes
                ? database_.MissingMarketTickCoverage(
                      resolved_query, "q")
                : std::vector<tradebox::core::TickCoverage>{};
        result.complete = result.missing_trade_ranges.empty() &&
                          result.missing_quote_ranges.empty();
        if (!result.complete)
            result.error =
                "No market-data credentials available for missing ranges";
        return result;
    }
    const std::string feed = AlpacaFeedName(query.feed);
    const auto fetch_kind =
        [&](std::string_view kind,
            const std::vector<tradebox::core::TickCoverage>& ranges) {
            const std::string resource =
                kind == "t" ? "trades" : "quotes";
            for (const auto& range : ranges) {
                std::string page_token;
                std::unordered_set<std::string> seen_tokens;
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
                    const HttpResult response = Get(
                        rest_transport_, L"data.alpaca.markets",
                        Wide(path), credentials,
                        tradebox::broker::alpaca::RestPriority::Interactive);
                    if (response.status != 200) {
                        succeeded = false;
                        result.error =
                            "Historical " + resource + " failed: " +
                            (!response.error.empty() ? response.error
                                                     : response.body);
                        break;
                    }
                    const auto envelope =
                        tradebox::broker::alpaca::
                            ValidateHistoricalPage(
                                response.body, resource);
                    if (!envelope) {
                        succeeded = false;
                        result.error =
                            "Historical " + resource +
                            " page rejected: " +
                            envelope.error();
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
                            if (event_time_ns < range.start_ns ||
                                event_time_ns >= range.end_ns)
                                throw std::runtime_error(
                                    "Historical " + resource +
                                    " item is outside the requested "
                                    "half-open range");
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
                            if (!database_.QueueMarketDataEvent(
                                    feed, std::move(source_id),
                                    std::move(event))) {
                                succeeded = false;
                                result.error =
                                    "Historical " + resource +
                                    " persistence queue rejected an item";
                                break;
                            }
                        }
                    }
                    if (!succeeded) break;
                    if (auto flushed =
                            database_.FlushQueuedWrites();
                        !flushed) {
                        succeeded = false;
                        result.error =
                            "Historical " + resource +
                            " persistence failed: " +
                            flushed.error();
                        break;
                    }
                    // Alpaca returns JSON null, rather than an empty string,
                    // for the final page.
                    const std::string next =
                        envelope->next_page_token;
                    if (!next.empty() &&
                        !seen_tokens.insert(next).second) {
                        succeeded = false;
                        result.error =
                            "Historical " + resource +
                            " pagination repeated a page token";
                        break;
                    }
                    page_token = next;
                } while (!page_token.empty());
                if (succeeded) {
                    const auto covered =
                        database_.MarkMarketTickCoverage(
                            resolved_query, kind, range);
                    if (!covered) {
                        result.error =
                            "Historical " + resource +
                            " coverage persistence failed: " +
                            covered.error();
                    }
                }
            }
        };
    if (query.include_trades)
        fetch_kind(
            "t", database_.MissingMarketTickCoverage(
                     resolved_query, "t"));
    if (query.include_quotes)
        fetch_kind(
            "q", database_.MissingMarketTickCoverage(
                     resolved_query, "q"));

    const std::string fetch_error = result.error;
    result = database_.LoadMarketTicks(resolved_query);
    if (!fetch_error.empty()) result.error = fetch_error;
    const auto live = market_data_view_.Snapshot(query.symbol);
    std::unordered_map<std::string, std::size_t> trade_ids;
    for (std::size_t index = 0; index < result.trades.size(); ++index)
        if (!result.trades[index].trade_id.empty())
            trade_ids[result.trades[index].trade_id] = index;
    for (const auto& trade : live.trades) {
        if (!query.include_trades ||
            trade.event_time_ns < query.start_ns ||
            trade.event_time_ns >= query.end_ns)
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
        live.latest_quote->event_time_ns < query.end_ns &&
        (result.quotes.empty() ||
         result.quotes.back().event_time_ns <
             live.latest_quote->event_time_ns))
        result.quotes.push_back(*live.latest_quote);
    result.missing_trade_ranges =
        query.include_trades
            ? database_.MissingMarketTickCoverage(
                  resolved_query, "t")
            : std::vector<tradebox::core::TickCoverage>{};
    result.missing_quote_ranges =
        query.include_quotes
            ? database_.MissingMarketTickCoverage(
                  resolved_query, "q")
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
        rest_transport_, L"data.alpaca.markets",
        Wide("/v2/stocks/snapshots?symbols=" +
             UrlEncode(joined) + "&feed=" + feed),
        credentials,
        tradebox::broker::alpaca::RestPriority::Interactive);
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
        std::unordered_set<std::string> seeded_trade_symbols;
        const auto ingest_trade = [&](const std::string& symbol,
                                       const json& item) {
            if (!item.is_object() || !item.contains("t") ||
                !item.contains("p") || !item.contains("s") ||
                item["t"].is_null() || item["p"].is_null() ||
                item["s"].is_null())
                return false;
            const std::string instrument_id =
                InstrumentIdForSymbol(symbol);
            const std::string timestamp = String(item, "t");
            const std::int64_t event_time_ns =
                ParseTimestampNs(timestamp);
            if (timestamp.empty() || event_time_ns <= 0) return false;
            const std::int64_t received_at_ms = WallClockNowMs();
            const std::string trade_id = Identifier(item, "i");
            auto event = tradebox::core::ShareMarketDataEvent(
                tradebox::core::TradeReceived{
                    .trade = {
                        .instrument_id = instrument_id,
                        .symbol = symbol,
                        .trade_id = trade_id,
                        .price = RequiredDecimal(item, "p"),
                        .size = RequiredDecimal(item, "s"),
                        .exchange = String(item, "x"),
                        .conditions = StringArray(item, "c"),
                        .tape = String(item, "z"),
                        .broker_timestamp = timestamp,
                        .event_time_ns = event_time_ns,
                        .received_at_ms = received_at_ms,
                    },
                });
            if (!database_.QueueMarketDataEvent(
                    feed,
                    symbol + ":" + std::to_string(event_time_ns) + ":" +
                        trade_id,
                    event) &&
                !persistence_queue_warning_emitted_.exchange(true))
                events_.Push(OperationalEvent(
                    OperationalComponent::Persistence,
                    OperationalState::Degraded,
                    OperationalSeverity::Critical,
                    "Market-data persistence queue rejected an event",
                    OperationalReason::QueueOverload));
            market_data_.Ingest(std::move(event));
            return true;
        };
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
                if (!database_.QueueMarketDataEvent(
                        feed,
                        symbol + ":q:" +
                            std::to_string(event_time_ns) + ":" +
                            StablePayloadId(identity),
                        event) &&
                    !persistence_queue_warning_emitted_.exchange(true))
                    events_.Push(OperationalEvent(
                        OperationalComponent::Persistence,
                        OperationalState::Degraded,
                        OperationalSeverity::Critical,
                        "Market-data persistence queue rejected an event",
                        OperationalReason::QueueOverload));
                market_data_.Ingest(std::move(event));
            }
            if (snapshot.contains("latestTrade") &&
                snapshot["latestTrade"].is_object()) {
                if (ingest_trade(symbol, snapshot["latestTrade"]))
                    seeded_trade_symbols.insert(symbol);
            }
        }

        std::vector<std::string> missing_trade_symbols;
        for (const std::string& symbol : symbols)
            if (!seeded_trade_symbols.contains(symbol))
                missing_trade_symbols.push_back(symbol);
        if (!missing_trade_symbols.empty()) {
            std::string missing_joined;
            for (const std::string& symbol : missing_trade_symbols) {
                if (!missing_joined.empty()) missing_joined += ',';
                missing_joined += symbol;
            }
            const HttpResult latest_trades = Get(
                rest_transport_, L"data.alpaca.markets",
                Wide("/v2/stocks/trades/latest?symbols=" +
                     UrlEncode(missing_joined) + "&feed=" + feed),
                credentials,
                tradebox::broker::alpaca::RestPriority::Interactive);
            if (latest_trades.status == 200) {
                const json value = json::parse(latest_trades.body);
                if (value.contains("trades") &&
                    value["trades"].is_object()) {
                    for (const std::string& symbol : missing_trade_symbols) {
                        if (!value["trades"].contains(symbol)) continue;
                        if (ingest_trade(symbol, value["trades"][symbol]))
                            seeded_trade_symbols.insert(symbol);
                    }
                }
            } else {
                events_.Push({
                    UiEventType::Status, {},
                    "Latest trade seed failed: " +
                        (!latest_trades.error.empty()
                             ? latest_trades.error
                             : latest_trades.body)});
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
    const HttpResult assets_response = Get(
        rest_transport_, host,
        L"/v2/assets?status=active&asset_class=us_equity", credentials);
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
    rest_transport_.CancelPending();
    // Abort the REST transport's WinHTTP session so that all in-flight HTTP
    // operations fail immediately. This prevents AccountRefreshLoop,
    // MarketClockLoop, and transport worker threads from blocking on slow
    // network calls during shutdown (which could otherwise take 10-20 seconds
    // due to WinHTTP timeouts).
    rest_transport_.Abort();
    HINTERNET socket = websocket_.exchange(nullptr);
    if (socket) WinHttpCloseHandle(socket);
    HINTERNET account_socket = account_websocket_.exchange(nullptr);
    if (account_socket) WinHttpCloseHandle(account_socket);
    if (stream_thread_.joinable()) stream_thread_.join();
    if (account_stream_thread_.joinable()) account_stream_thread_.join();
    JoinWorkers();
    WaitForBackgroundIdle();
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

bool AlpacaService::SubmitBackground(std::function<void()> task) {
    {
        std::scoped_lock lock(background_mutex_);
        if (background_stopping_ || background_tasks_.size() >= 128) {
            ++background_rejected_;
            return false;
        }
        background_tasks_.push_back(std::move(task));
    }
    background_ready_.notify_one();
    return true;
}

void AlpacaService::BackgroundWorkerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(background_mutex_);
            background_ready_.wait(lock, [this] {
                return background_stopping_ ||
                       !background_tasks_.empty();
            });
            if (background_tasks_.empty() && background_stopping_)
                return;
            task = std::move(background_tasks_.front());
            background_tasks_.pop_front();
            ++background_active_;
        }
        try {
            task();
        } catch (const std::exception& error) {
            events_.Push(
                {UiEventType::Status, {},
                 "Background task failed: " +
                     std::string(error.what())});
        } catch (...) {
            events_.Push(
                {UiEventType::Status, {},
                 "Background task failed with an unknown error"});
        }
        {
            std::scoped_lock lock(background_mutex_);
            --background_active_;
            if (background_tasks_.empty() && background_active_ == 0)
                background_idle_.notify_all();
        }
    }
}

void AlpacaService::WaitForBackgroundIdle() {
    std::unique_lock lock(background_mutex_);
    background_idle_.wait(lock, [this] {
        return background_tasks_.empty() && background_active_ == 0;
    });
}

void AlpacaService::PublishCoreEvent(tradebox::core::BrokerEvent event) {
    const bool may_change_positions =
        event.kind ==
            tradebox::core::BrokerEventKind::PositionsSnapshot ||
        event.kind ==
            tradebox::core::BrokerEventKind::TradeUpdate;
    if (event.generation.value == 0)
        event.generation =
            tradebox::core::ConnectionGeneration{active_generation_.load()};
    if (auto result = core_.Ingest(std::move(event)); !result) {
        events_.Push(
            {UiEventType::Status, {},
             "Trading core rejected broker event: " +
                 result.error().message});
    } else if (may_change_positions) {
        const auto snapshot = core_.Snapshot();
        for (const auto& position : snapshot.positions) {
            const std::string& identifier =
                position.asset_id.empty()
                    ? position.symbol
                    : position.asset_id;
            core_.ApplyMarketData(
                market_data_view_.Snapshot(identifier));
        }
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
    auto result = CommandResult(Request(
        rest_transport_, L"POST", host, L"/v2/orders", credentials, *body,
        tradebox::broker::alpaca::RestPriority::Command));
    if (result.disposition !=
        tradebox::broker::BrokerCommandDisposition::Rejected) {
        orders_dirty_ = true;
        account_dirty_ = true;
    }
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
        rest_transport_, L"DELETE", host,
        Wide("/v2/orders/" + UrlEncode(order_id)), credentials, {},
        tradebox::broker::alpaca::RestPriority::Command));
    if (result.disposition !=
        tradebox::broker::BrokerCommandDisposition::Rejected) {
        orders_dirty_ = true;
        account_dirty_ = true;
    }
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
        rest_transport_, L"PATCH", host,
        Wide("/v2/orders/" + UrlEncode(order_id)), credentials, *body,
        tradebox::broker::alpaca::RestPriority::Command));
    if (result.disposition !=
        tradebox::broker::BrokerCommandDisposition::Rejected) {
        orders_dirty_ = true;
        account_dirty_ = true;
    }
    return result;
}

tradebox::broker::BrokerCommandResult AlpacaService::ClosePosition(
    const std::string& symbol_or_asset_id,
    const std::optional<tradebox::core::Decimal>& qty,
    const std::optional<tradebox::core::Decimal>& percentage) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (symbol_or_asset_id.empty() || (qty && percentage))
        return {
            .disposition =
                tradebox::broker::BrokerCommandDisposition::Rejected,
            .message = "A close target and at most one close amount are required",
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
    std::string path =
        "/v2/positions/" + UrlEncode(symbol_or_asset_id);
    if (qty)
        path += "?qty=" + UrlEncode(qty->ToString());
    else if (percentage)
        path += "?percentage=" + UrlEncode(percentage->ToString());
    auto result = CommandResult(Request(
        rest_transport_, L"DELETE", host, Wide(path), credentials, {},
        tradebox::broker::alpaca::RestPriority::Command));
    result.reconciliation_required = true;
    positions_dirty_ = true;
    orders_dirty_ = true;
    account_dirty_ = true;
    return result;
}

tradebox::broker::BrokerCommandResult AlpacaService::CloseAllPositions(
    bool cancel_open_orders) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
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
    auto result = BulkCommandResult(Request(
        rest_transport_, L"DELETE", host,
        cancel_open_orders
            ? L"/v2/positions?cancel_orders=true"
            : L"/v2/positions?cancel_orders=false",
        credentials, {},
        tradebox::broker::alpaca::RestPriority::Command));
    positions_dirty_ = true;
    orders_dirty_ = true;
    account_dirty_ = true;
    return result;
}

tradebox::broker::BrokerCommandResult AlpacaService::CancelAllOrders() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
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
    auto result = BulkCommandResult(Request(
        rest_transport_, L"DELETE", host, L"/v2/orders", credentials, {},
        tradebox::broker::alpaca::RestPriority::Command));
    orders_dirty_ = true;
    account_dirty_ = true;
    return result;
}

void AlpacaService::FetchAccount() {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    const std::wstring host = credentials.paper ? L"paper-api.alpaca.markets"
                                                 : L"api.alpaca.markets";
    const HttpResult result = Get(
        rest_transport_, host, L"/v2/account", credentials,
        tradebox::broker::alpaca::RestPriority::AccountSafety);
    if (result.status != 200) {
        events_.Push(OperationalEvent(
            OperationalComponent::Account,
            OperationalState::Failed,
            OperationalSeverity::Critical,
            "Account login failed: " +
                (!result.error.empty() ? result.error : result.body)));
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
        events_.Push(OperationalEvent(
            OperationalComponent::Account,
            OperationalState::Failed,
            OperationalSeverity::Critical,
            "Account JSON error: " + std::string(error.what())));
    }
}

void AlpacaService::FetchPositions() {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    const std::wstring host = credentials.paper ? L"paper-api.alpaca.markets"
                                                 : L"api.alpaca.markets";
    const HttpResult result = Get(
        rest_transport_, host, L"/v2/positions", credentials,
        tradebox::broker::alpaca::RestPriority::AccountSafety);
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
        const HttpResult result = Get(
            rest_transport_, host, Wide(path), credentials,
            tradebox::broker::alpaca::RestPriority::AccountSafety);
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
                ParseCoreOrderTree(item, core_orders);
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

void AlpacaService::FetchAccountActivities() {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    const std::wstring host =
        credentials.paper ? L"paper-api.alpaca.markets"
                          : L"api.alpaca.markets";
    const tradebox::core::CoreSnapshot snapshot = core_.Snapshot();
    if (!snapshot.account || snapshot.account->id.empty()) {
        if (!activities_identity_deferred_reported_) {
            events_.Push({UiEventType::Status, {},
                          "Account activities deferred until account "
                          "identity is available"});
            activities_identity_deferred_reported_ = true;
        }
        activities_dirty_ = true;
        return;
    }
    std::string page_token;
    const auto latest = database_.LoadAccountActivities({
        .account_id = snapshot.account->id,
        .maximum = 1,
    });
    const std::string after =
        latest.activities.empty()
            ? std::string{}
            : latest.activities.front().occurred_at;
    std::unordered_set<std::string> observed_tokens;
    std::size_t stored = 0;
    while (running_) {
        const HttpResult result = Get(
            rest_transport_, host,
            Wide(tradebox::broker::alpaca::BuildAccountActivitiesPath(
                page_token, after)),
            credentials);
        if (result.status != 200) {
            events_.Push(
                {UiEventType::Status, {},
                 "Account activities load failed: " +
                     (!result.error.empty() ? result.error
                                            : result.body)});
            return;
        }
        auto parsed =
            tradebox::broker::alpaca::ParseAccountActivityPage(
                result.body, snapshot.account->id, snapshot.orders,
                page_token);
        if (!parsed) {
            events_.Push({UiEventType::Status, {}, parsed.error()});
            return;
        }
        const auto write =
            database_.StoreAccountActivities(parsed->activities);
        if (!write) {
            events_.Push(
                {UiEventType::Status, {},
                 "Account activity persistence failed: " +
                     write.error()});
            return;
        }
        stored += write->inserted + write->revised;
        if (parsed->next_page_token.empty()) break;
        if (!observed_tokens.insert(parsed->next_page_token).second) {
            events_.Push(
                {UiEventType::Status, {},
                 "Account activity pagination stopped on repeated token"});
            return;
        }
        page_token = std::move(parsed->next_page_token);
    }
    if (stored != 0)
        events_.Push(
            {UiEventType::Status, {},
             "Account activity ledger updated: " +
                 std::to_string(stored) + " inserted or revised"});
}

void AlpacaService::AccountRefreshLoop() {
    auto next_account_refresh = std::chrono::steady_clock::now();
    auto next_safety_reconciliation = std::chrono::steady_clock::now();
    auto next_order_safety_reconciliation = std::chrono::steady_clock::now();
    auto next_activity_refresh = std::chrono::steady_clock::now();
    while (running_) {
        ReportPersistenceHealth();
        const auto now = std::chrono::steady_clock::now();
        if (account_dirty_.exchange(false) ||
            now >= next_account_refresh) {
            FetchAccount();
            next_account_refresh =
                now + tradebox::broker::alpaca::
                          kAccountReanchorInterval;
        }
        if (positions_dirty_.exchange(false) ||
            now >= next_safety_reconciliation) {
            FetchPositions();
            next_safety_reconciliation =
                now + tradebox::broker::alpaca::
                          kPositionReanchorInterval;
        }
        const bool stream_unavailable = !account_connected_.load();
        const bool order_safety_due =
            stream_unavailable && now >= next_order_safety_reconciliation;
        if ((orders_dirty_.exchange(false) || order_safety_due) && running_) {
            FetchOrders();
            next_order_safety_reconciliation =
                now + tradebox::broker::alpaca::
                          kOrderFallbackInterval;
        }
        if ((activities_dirty_.exchange(false) ||
             now >= next_activity_refresh) &&
            running_) {
            FetchAccountActivities();
            next_activity_refresh =
                now + tradebox::broker::alpaca::
                          kActivityRefreshInterval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void AlpacaService::FetchMarketClock() {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    const std::wstring host = credentials.paper ? L"paper-api.alpaca.markets"
                                                 : L"api.alpaca.markets";
    const HttpResult result =
        Get(rest_transport_, host, L"/v2/clock", credentials);
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
        const auto delay =
            tradebox::broker::alpaca::kMarketClockInterval;
        const auto tenths =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                delay)
                .count() /
            100;
        for (std::int64_t tenth_seconds = 0;
             tenth_seconds < tenths && running_; ++tenth_seconds)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void AlpacaService::FetchHistory(
    std::string symbol,
    tradebox::core::BarSeriesKey key,
    tradebox::core::BarRange requested_range,
    std::vector<tradebox::core::BarRange> reserved_ranges) {
    const AlpacaCredentials credentials = CredentialsSnapshot();
    bool request_succeeded = true;
    std::string request_error;
    try {
        for (const tradebox::core::BarRange range :
             reserved_ranges) {
            bool complete = true;
            std::string page_token;
            std::unordered_set<std::string> seen_tokens;
            do {
                const std::string path =
                    tradebox::broker::alpaca::
                        BuildHistoricalBarPath({
                            .symbol = symbol,
                            .key = key,
                            .range = range,
                            .page_token = page_token,
                            .limit = 10'000,
                        });
                const HttpResult response = Get(
                    rest_transport_, L"data.alpaca.markets",
                    Wide(path), credentials,
                    tradebox::broker::alpaca::RestPriority::Interactive);
                if (response.status != 200) {
                    complete = false;
                    request_succeeded = false;
                    request_error = !response.error.empty()
                                        ? response.error
                                        : response.body;
                    events_.Push({
                        UiEventType::Status,
                        symbol,
                        "History failed for " + symbol + ": " +
                            (!response.error.empty()
                                 ? response.error
                                 : response.body),
                    });
                    break;
                }

                const json value = json::parse(response.body);
                const auto envelope =
                    tradebox::broker::alpaca::
                        ValidateHistoricalPage(
                            response.body, "bars");
                if (!envelope) {
                    complete = false;
                    request_succeeded = false;
                    request_error = envelope.error();
                    events_.Push({
                        UiEventType::Status,
                        symbol,
                        "History page rejected for " +
                            symbol + ": " +
                            envelope.error(),
                    });
                    break;
                }
                std::vector<tradebox::core::MarketBar>
                    page_bars;
                const auto items = value.find("bars");
                if (items != value.end() &&
                    items->is_array()) {
                    page_bars.reserve(items->size());
                    for (const auto& item : *items) {
                        const std::int64_t start_ns =
                            ParseTimestampNs(
                                item.value("t", ""));
                        if (start_ns < range.start_ns ||
                            start_ns >= range.end_ns)
                            throw std::runtime_error(
                                "Historical bar is outside the "
                                "requested half-open range");
                        page_bars.push_back({
                            .start_ns = start_ns,
                            .open =
                                RequiredDecimal(item, "o"),
                            .high =
                                RequiredDecimal(item, "h"),
                            .low =
                                RequiredDecimal(item, "l"),
                            .close =
                                RequiredDecimal(item, "c"),
                            .volume =
                                RequiredDecimal(item, "v"),
                            .within_bar_vwap =
                                OptionalDecimal(item, "vw"),
                            .trade_count =
                                static_cast<std::uint64_t>(
                                    std::max(
                                        0.0,
                                        Number(item, "n"))),
                            .source =
                                tradebox::core::BarSource::
                                    ProviderHistorical,
                            .state =
                                tradebox::core::BarState::
                                    Finalized,
                        });
                    }
                }
                tradebox::core::BarUpsertBatch page{
                    .key = key,
                    .symbol = symbol,
                    .bars = std::move(page_bars),
                };
                const auto stored_page =
                    database_.StoreProviderBars(page);
                if (!stored_page) {
                    complete = false;
                    request_succeeded = false;
                    request_error = stored_page.error();
                    events_.Push(OperationalEvent(
                        OperationalComponent::Persistence,
                        OperationalState::Failed,
                        OperationalSeverity::Critical,
                        "Historical bar persistence failed for " +
                            symbol + ": " + stored_page.error(),
                        OperationalReason::PersistenceFailure));
                    break;
                }
                bars_.Upsert(std::move(page));

                const std::string next =
                    envelope->next_page_token;
                if (!next.empty() &&
                    !seen_tokens.insert(next).second) {
                    complete = false;
                    request_succeeded = false;
                    request_error =
                        "History pagination repeated a page token";
                    events_.Push({
                        UiEventType::Status,
                        symbol,
                        "History pagination repeated a page token",
                    });
                    break;
                }
                page_token = next;
            } while (!page_token.empty() && running_);

            if (!running_) {
                complete = false;
                request_succeeded = false;
                request_error = "History request was interrupted";
            }
            if (complete) {
                tradebox::core::BarUpsertBatch coverage{
                    .key = key,
                    .symbol = symbol,
                    .covered_range = range,
                };
                const auto stored_coverage =
                    database_.StoreProviderBars(coverage);
                if (!stored_coverage) {
                    request_succeeded = false;
                    request_error = stored_coverage.error();
                    events_.Push(OperationalEvent(
                        OperationalComponent::Persistence,
                        OperationalState::Failed,
                        OperationalSeverity::Critical,
                        "Historical bar coverage persistence failed for " +
                            symbol + ": " + stored_coverage.error(),
                        OperationalReason::PersistenceFailure));
                } else {
                    bars_.Upsert(std::move(coverage));
                }
            }
        }
    } catch (const std::exception& error) {
        request_succeeded = false;
        request_error = error.what();
        events_.Push({UiEventType::Status, symbol,
                      "History JSON error: " + std::string(error.what())});
    }
    in_flight_bar_ranges_.Release(key, reserved_ranges);
    PublishCachedHistory(symbol, key, requested_range);
    if (history_status_)
        history_status_->Publish({
            .key = key,
            .range = requested_range,
            .state = request_succeeded
                         ? tradebox::broker::HistoryRequestState::Succeeded
                         : tradebox::broker::HistoryRequestState::Failed,
            .message = std::move(request_error),
        });
}

void AlpacaService::PublishCachedHistory(
    const std::string& symbol,
    const tradebox::core::BarSeriesKey& key,
    tradebox::core::BarRange range) {
    StoredBarSeries stored =
        database_.LoadProviderBars(key, range);
    std::vector<Bar> display_bars;
    display_bars.reserve(stored.bars.size());
    for (const auto& bar : stored.bars) {
        display_bars.push_back({
            .timestamp_ms = bar.start_ns / 1'000'000,
            .open = bar.open.ToDisplayDouble(),
            .high = bar.high.ToDisplayDouble(),
            .low = bar.low.ToDisplayDouble(),
            .close = bar.close.ToDisplayDouble(),
            .volume = bar.volume.ToDisplayDouble(),
        });
    }

    std::optional<tradebox::core::BarRange> first_coverage;
    if (!stored.coverage.empty()) {
        first_coverage = stored.coverage.front();
        stored.coverage.erase(stored.coverage.begin());
    }
    bars_.Upsert({
        .key = key,
        .symbol = symbol,
        .bars = std::move(stored.bars),
        .covered_range = first_coverage,
    });
    for (const auto coverage : stored.coverage)
        bars_.Upsert({
            .key = key,
            .symbol = symbol,
            .covered_range = coverage,
        });

    if (key.timeframe == "1Day")
        database_.StoreBars(symbol, display_bars);
    UiEvent event;
    event.type = UiEventType::HistoricalBars;
    event.symbol = symbol;
    event.timeframe = key.timeframe;
    event.bars = std::move(display_bars);
    events_.Push(std::move(event));
}

void AlpacaService::ScheduleMarketGapBackfill(
    std::int64_t disconnected_at_ns,
    std::int64_t reconnected_at_ns,
    std::vector<std::string> symbols) {
    if (!tradebox::broker::alpaca::HasRecoverableMinuteGap(
            disconnected_at_ns, reconnected_at_ns))
        return;
    constexpr std::int64_t minute_ns =
        60LL * 1'000'000'000;
    const tradebox::core::BarRange range{
        .start_ns =
            disconnected_at_ns / minute_ns * minute_ns,
        .end_ns =
            reconnected_at_ns / minute_ns * minute_ns,
    };
    if (!SubmitBackground(
            [this, symbols = std::move(symbols), range] {
                for (const auto& symbol : symbols) {
                    if (!running_) return;
                    const tradebox::core::BarSeriesKey key{
                        .instrument_id =
                            InstrumentIdForSymbol(symbol),
                        .feed = market_data_feed_,
                        .timeframe = "1Min",
                        .adjustment =
                            tradebox::core::BarAdjustment::Raw,
                    };
                    if (key.instrument_id.empty()) continue;
                    const StoredBarSeries stored =
                        database_.LoadProviderBars(key, range);
                    const auto missing =
                        tradebox::core::MissingBarRanges(
                            stored.coverage, range);
                    auto reserved =
                        in_flight_bar_ranges_.Reserve(key, missing);
                    if (reserved.empty()) continue;
                    FetchHistory(
                        symbol, key, range, std::move(reserved));
                }
            })) {
        std::int64_t expected = 0;
        market_gap_started_ns_.compare_exchange_strong(
            expected, disconnected_at_ns);
        events_.Push({
            UiEventType::Status,
            {},
            "Market gap backfill rejected: background queue full",
        });
    }
}

bool AlpacaService::WaitForReconnect(
    std::chrono::milliseconds delay) {
    return tradebox::broker::alpaca::
        InterruptibleReconnectWait(running_, delay);
}

void AlpacaService::StreamLoop() {
    tradebox::broker::alpaca::ReconnectBackoff backoff(
        tradebox::broker::alpaca::StreamChannel::MarketData);
    while (running_) {
        const auto started = std::chrono::steady_clock::now();
        const bool ready = RunMarketStreamSession();
        if (!running_) break;
        const auto lifetime =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
        const auto delay = backoff.NextDelay(ready, lifetime);
        UiEvent reconnect = OperationalEvent(
            OperationalComponent::MarketDataStream,
            OperationalState::Reconnecting,
            OperationalSeverity::Warning,
            "Market stream reconnect scheduled",
            OperationalReason::UnexpectedDisconnect);
        reconnect.retry_attempt =
            static_cast<std::uint32_t>(backoff.failure_count());
        reconnect.retry_in_ms = delay.count();
        events_.Push(std::move(reconnect));
        if (!WaitForReconnect(delay)) break;
    }
}

bool AlpacaService::RunMarketStreamSession() {
    bool reached_ready_state = false;
    events_.Push(OperationalEvent(
        OperationalComponent::MarketDataStream,
        OperationalState::Connecting,
        OperationalSeverity::Informational,
        "Connecting to market stream"));
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
    if (!session) return false;
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
    DWORD security_policy_error = NO_ERROR;
    if (!request ||
        !ApplySecureRequestPolicy(
            request, security_policy_error) ||
        !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        const bool policy_failed =
            security_policy_error != NO_ERROR;
        const std::string failure_message =
            policy_failed
                ? "Market stream security policy failed (" +
                      std::to_string(security_policy_error) + ")"
                : "Market stream connection failed (" +
                      std::to_string(GetLastError()) + ")";
        events_.Push(OperationalEvent(
            OperationalComponent::MarketDataStream,
            OperationalState::Failed,
            OperationalSeverity::Critical,
            failure_message,
            policy_failed
                ? OperationalReason::SecurityPolicyFailure
                : OperationalReason::TransportFailure));
        market_data_.Ingest(tradebox::core::MarketStreamChanged{
            .status = tradebox::core::MarketStreamStatus::Error,
            .feed = feed,
            .message = failure_message,
            .received_at_ms = WallClockNowMs(),
        });
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    if (!socket) {
        events_.Push(OperationalEvent(
            OperationalComponent::MarketDataStream,
            OperationalState::Failed,
            OperationalSeverity::Critical,
            "Market WebSocket upgrade failed",
            OperationalReason::UpgradeFailure));
        market_data_.Ingest(tradebox::core::MarketStreamChanged{
            .status = tradebox::core::MarketStreamStatus::Error,
            .feed = feed,
            .message = "Market WebSocket upgrade failed",
            .received_at_ms = WallClockNowMs(),
        });
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }
    websocket_ = socket;
    DWORD keepalive_interval = static_cast<DWORD>(
        tradebox::broker::alpaca::StreamSupervisionPolicy{}
            .keepalive_interval.count());
    WinHttpSetOption(
        socket, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL,
        &keepalive_interval, sizeof(keepalive_interval));
    std::string auth =
        tradebox::broker::alpaca::BuildAuthenticationMessage(
            credentials.key, credentials.secret);
    DWORD auth_send_error = NO_ERROR;
    bool auth_sent = false;
    {
        std::scoped_lock send_lock(websocket_send_mutex_);
        auth_sent = SendText(
            socket, auth, &auth_send_error);
    }
    ClearSensitiveString(auth);
    if (!auth_sent) {
        const std::string failure_message =
            "Market stream authentication send failed (" +
            std::to_string(auth_send_error) + ")";
        events_.Push(OperationalEvent(
            OperationalComponent::MarketDataStream,
            OperationalState::Failed,
            OperationalSeverity::Critical,
            failure_message,
            OperationalReason::TransportFailure));
        market_data_.Ingest(
            tradebox::core::MarketStreamChanged{
                .status = tradebox::core::
                    MarketStreamStatus::Error,
                .feed = feed,
                .message = failure_message,
                .received_at_ms = WallClockNowMs(),
            });
        websocket_ = nullptr;
        WinHttpCloseHandle(socket);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    std::string message;
    std::vector<char> buffer(64 * 1024);
    DWORD receive_error = NO_ERROR;
    bool protocol_failed = false;
    std::size_t subscription_mismatches = 0;
    while (running_ && websocket_.load() == socket) {
        DWORD bytes = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD rc = WinHttpWebSocketReceive(
            socket, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, &type);
        if (rc != NO_ERROR) {
            receive_error = rc;
            break;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        const bool fragmented =
            type ==
                WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
            type ==
                WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
        const std::string_view received(
            buffer.data(), bytes);
        const bool direct_frame =
            message.empty() && !fragmented;
        if ((!direct_frame &&
             !tradebox::broker::alpaca::AppendInboundPayload(
                 message, received,
                 tradebox::broker::alpaca::
                     kMaximumMarketStreamMessageBytes)) ||
            (direct_frame &&
             received.size() >
                 tradebox::broker::alpaca::
                     kMaximumMarketStreamMessageBytes)) {
            const std::string failure_message =
                "Market stream message exceeded " +
                std::to_string(
                    tradebox::broker::alpaca::
                        kMaximumMarketStreamMessageBytes) +
                "-byte safety limit";
            events_.Push(OperationalEvent(
                OperationalComponent::MarketDataStream,
                OperationalState::Failed,
                OperationalSeverity::Critical,
                failure_message,
                OperationalReason::PayloadLimitExceeded));
            market_data_.Ingest(
                tradebox::core::MarketStreamChanged{
                    .status = tradebox::core::
                        MarketStreamStatus::Error,
                    .feed = feed,
                    .message = failure_message,
                    .received_at_ms = WallClockNowMs(),
                });
            protocol_failed = true;
            break;
        }
        if (fragmented) continue;
        const std::string_view frame_payload =
            direct_frame ? received
                         : std::string_view(message);
        try {
            auto frame = tradebox::broker::alpaca::DecodeMarketFrame(
                frame_payload, WallClockNowMs(),
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
                    }
                    if (!desired.empty()) {
                        std::scoped_lock send_lock(
                            websocket_send_mutex_);
                        SendText(
                            socket,
                            Subscription("subscribe", desired));
                    }
                    events_.Push(OperationalEvent(
                        OperationalComponent::MarketDataStream,
                        OperationalState::Authenticated,
                        OperationalSeverity::Informational,
                        "Market stream authenticated"));
                } else if (control.type ==
                           StreamControlType::Error) {
                    const std::string error_message =
                        "Stream error: " + control.message;
                    events_.Push(OperationalEvent(
                        OperationalComponent::MarketDataStream,
                        OperationalState::Failed,
                        OperationalSeverity::Critical,
                        error_message,
                        connected_
                            ? OperationalReason::TransportFailure
                            : OperationalReason::AuthenticationFailure));
                    market_data_.Ingest(
                        tradebox::core::MarketStreamChanged{
                            .status = tradebox::core::
                                MarketStreamStatus::Error,
                            .feed = feed,
                            .message = error_message,
                            .received_at_ms = WallClockNowMs(),
                        });
                    protocol_failed = true;
                } else {
                    std::vector<std::string> desired;
                    {
                        std::scoped_lock subscription_lock(
                            subscription_mutex_);
                        desired = desired_symbols_;
                        subscribed_symbols_ =
                            control.trade_symbols;
                    }
                    const auto subscription_recovery =
                        tradebox::broker::alpaca::
                            EvaluateSubscription(
                                desired,
                                control.trade_symbols,
                                control.quote_symbols,
                                control.status_symbols,
                                subscription_mismatches + 1);
                    const bool exact_subscription =
                        subscription_recovery ==
                        tradebox::broker::alpaca::
                            SubscriptionRecovery::Ready;
                    if (exact_subscription) {
                        subscription_mismatches = 0;
                    } else {
                        ++subscription_mismatches;
                        std::scoped_lock send_lock(
                            websocket_send_mutex_);
                        std::vector<std::string> acknowledged =
                            control.trade_symbols;
                        acknowledged.insert(
                            acknowledged.end(),
                            control.quote_symbols.begin(),
                            control.quote_symbols.end());
                        acknowledged.insert(
                            acknowledged.end(),
                            control.status_symbols.begin(),
                            control.status_symbols.end());
                        std::ranges::sort(acknowledged);
                        acknowledged.erase(
                            std::unique(
                                acknowledged.begin(),
                                acknowledged.end()),
                            acknowledged.end());
                        if (!acknowledged.empty())
                            SendText(
                                socket,
                                Subscription(
                                    "unsubscribe",
                                    acknowledged));
                        if (!desired.empty())
                            SendText(
                                socket,
                                Subscription("subscribe", desired));
                        if (subscription_recovery ==
                            tradebox::broker::alpaca::
                                SubscriptionRecovery::Restart)
                            protocol_failed = true;
                    }
                    const std::size_t subscribed =
                        control.trade_symbols.size();
                    events_.Push(OperationalEvent(
                        OperationalComponent::MarketDataStream,
                        exact_subscription
                            ? OperationalState::Subscribed
                            : OperationalState::Degraded,
                        exact_subscription
                            ? OperationalSeverity::Informational
                            : OperationalSeverity::Warning,
                        exact_subscription
                            ? "Market subscription active: " +
                                  std::to_string(subscribed) +
                                  " symbols"
                            : "Market subscription does not match "
                              "the desired symbol set",
                        exact_subscription
                            ? OperationalReason::None
                            : OperationalReason::SubscriptionMismatch));
                    market_data_.Ingest(
                        tradebox::core::MarketStreamChanged{
                            .status =
                                exact_subscription
                                    ? tradebox::core::
                                          MarketStreamStatus::Subscribed
                                    : tradebox::core::
                                          MarketStreamStatus::Error,
                            .feed = feed,
                            .trade_symbols =
                                control.trade_symbols,
                            .quote_symbols =
                                control.quote_symbols,
                            .status_symbols =
                                control.status_symbols,
                            .message =
                                exact_subscription
                                    ? std::string(feed_name) +
                                          " trades and quotes subscribed"
                                    : std::string(feed_name) +
                                          " subscription mismatch",
                            .received_at_ms = WallClockNowMs(),
                        });
                    if (exact_subscription) {
                        reached_ready_state = true;
                        const std::int64_t gap_start =
                            market_gap_started_ns_.exchange(0);
                        const std::int64_t now_ns =
                            WallClockNowMs() * 1'000'000;
                        ScheduleMarketGapBackfill(
                            gap_start, now_ns, desired);
                    }
                    if (exact_subscription &&
                        !control.trade_symbols.empty()) {
                        if (!SubmitBackground(
                                [this,
                                 symbols = control.trade_symbols] {
                                    SeedLatestSnapshots(symbols);
                                }))
                            events_.Push(
                                {UiEventType::Status, {},
                                 "Snapshot seed rejected: "
                                 "background queue full"});
                    }
                }
            }
            std::vector<QueuedMarketDataEvent>
                persistence_events;
            std::vector<tradebox::core::MarketDataEventPtr>
                projection_events;
            persistence_events.reserve(frame.items.size());
            projection_events.reserve(frame.items.size());
            for (auto& decoded : frame.items) {
                if (decoded.market_tick) {
                    persistence_events.push_back({
                        .source_event_id =
                            std::move(decoded.source_event_id),
                        .event = decoded.market_event,
                    });
                }
                if (decoded.market_event)
                    projection_events.push_back(
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
                                    : bar.kind == "u"
                                          ? tradebox::core::
                                                BarState::Revised
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
            if (!database_.QueueMarketDataEvents(
                    sip ? "sip" : "iex",
                    std::move(persistence_events)) &&
                !persistence_queue_warning_emitted_.exchange(true))
                events_.Push(OperationalEvent(
                    OperationalComponent::Persistence,
                    OperationalState::Degraded,
                    OperationalSeverity::Critical,
                    "Market-data persistence queue rejected a "
                    "complete frame",
                    OperationalReason::QueueOverload));
            market_data_.IngestBatch(
                std::move(projection_events));
            if (protocol_failed) break;
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
        events_.Push(OperationalEvent(
            OperationalComponent::MarketDataStream,
            OperationalState::Disconnected,
            OperationalSeverity::Critical,
            "Market stream disconnected",
            receive_error == ERROR_WINHTTP_TIMEOUT
                ? OperationalReason::SilenceTimeout
                : OperationalReason::UnexpectedDisconnect));
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
    if (running_ && reached_ready_state) {
        std::int64_t expected = 0;
        market_gap_started_ns_.compare_exchange_strong(
            expected, WallClockNowMs() * 1'000'000);
    }
    return reached_ready_state;
}

void AlpacaService::AccountStreamLoop() {
    tradebox::broker::alpaca::ReconnectBackoff backoff(
        tradebox::broker::alpaca::StreamChannel::Account);
    while (running_) {
        const auto started = std::chrono::steady_clock::now();
        const bool ready = RunAccountStreamSession();
        if (!running_) break;
        const auto lifetime =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
        const auto delay = backoff.NextDelay(ready, lifetime);
        UiEvent reconnect = OperationalEvent(
            OperationalComponent::AccountStream,
            OperationalState::Reconnecting,
            OperationalSeverity::Warning,
            "Account stream reconnect scheduled",
            OperationalReason::UnexpectedDisconnect);
        reconnect.retry_attempt =
            static_cast<std::uint32_t>(backoff.failure_count());
        reconnect.retry_in_ms = delay.count();
        events_.Push(std::move(reconnect));
        if (!WaitForReconnect(delay)) break;
    }
}

bool AlpacaService::RunAccountStreamSession() {
    bool reached_ready_state = false;
    const std::uint64_t stream_attempt =
        account_stream_attempt_.fetch_add(1) + 1;
    events_.Push(OperationalEvent(
        OperationalComponent::AccountStream,
        OperationalState::Connecting,
        OperationalSeverity::Informational,
        "Connecting to account stream"));
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
    DWORD security_policy_error = NO_ERROR;
    if (!session || !request ||
        !ApplySecureRequestPolicy(
            request, security_policy_error) ||
        !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr,
                          0) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        const bool policy_failed =
            security_policy_error != NO_ERROR;
        const std::string failure_message =
            policy_failed
                ? "Account stream security policy failed (" +
                      std::to_string(security_policy_error) + ")"
                : "Account stream connection failed (" +
                      std::to_string(GetLastError()) + ")";
        events_.Push(OperationalEvent(
            OperationalComponent::AccountStream,
            OperationalState::Failed,
            OperationalSeverity::Critical,
            failure_message,
            policy_failed
                ? OperationalReason::SecurityPolicyFailure
                : OperationalReason::TransportFailure));
        PublishCoreEvent(tradebox::core::BrokerEvent{
            .kind = tradebox::core::BrokerEventKind::Failure,
            .raw_payload = "{}",
            .message = failure_message,
        });
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    if (!socket) {
        events_.Push(OperationalEvent(
            OperationalComponent::AccountStream,
            OperationalState::Failed,
            OperationalSeverity::Critical,
            "Account WebSocket upgrade failed",
            OperationalReason::UpgradeFailure));
        PublishCoreEvent(tradebox::core::BrokerEvent{
            .kind = tradebox::core::BrokerEventKind::Failure,
            .raw_payload = "{}",
            .message = "Account WebSocket upgrade failed",
        });
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }
    account_websocket_ = socket;
    DWORD keepalive_interval = static_cast<DWORD>(
        tradebox::broker::alpaca::StreamSupervisionPolicy{}
            .keepalive_interval.count());
    WinHttpSetOption(
        socket, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL,
        &keepalive_interval, sizeof(keepalive_interval));
    std::string auth =
        tradebox::broker::alpaca::BuildAuthenticationMessage(
            credentials.key, credentials.secret);
    const auto auth_started = std::chrono::steady_clock::now();
    DWORD auth_send_error = NO_ERROR;
    const bool auth_sent =
        SendText(socket, auth, &auth_send_error);
    ClearSensitiveString(auth);
    if (!auth_sent) {
        const std::string failure_message =
            "Account stream authentication send failed (" +
            std::to_string(auth_send_error) + ")";
        events_.Push(OperationalEvent(
            OperationalComponent::AccountStream,
            OperationalState::Failed,
            OperationalSeverity::Critical,
            failure_message,
            OperationalReason::TransportFailure));
        PublishCoreEvent(tradebox::core::BrokerEvent{
            .kind = tradebox::core::BrokerEventKind::Failure,
            .raw_payload = "{}",
            .message = failure_message,
        });
        account_websocket_ = nullptr;
        WinHttpCloseHandle(socket);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    std::string message;
    std::vector<char> buffer(64 * 1024);
    bool protocol_failed = false;
    DWORD receive_error = NO_ERROR;
    while (running_ && account_websocket_.load() == socket) {
        DWORD bytes = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD rc = WinHttpWebSocketReceive(
            socket, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes,
            &type);
        if (rc != NO_ERROR) {
            receive_error = rc;
            break;
        }
        if (!tradebox::broker::alpaca::AppendInboundPayload(
                message,
                std::string_view(buffer.data(), bytes),
                tradebox::broker::alpaca::
                    kMaximumAccountStreamMessageBytes)) {
            const std::string failure_message =
                "Account stream message exceeded " +
                std::to_string(
                    tradebox::broker::alpaca::
                        kMaximumAccountStreamMessageBytes) +
                "-byte safety limit";
            events_.Push(OperationalEvent(
                OperationalComponent::AccountStream,
                OperationalState::Failed,
                OperationalSeverity::Critical,
                failure_message,
                OperationalReason::PayloadLimitExceeded));
            PublishCoreEvent(tradebox::core::BrokerEvent{
                .kind =
                    tradebox::core::BrokerEventKind::Failure,
                .raw_payload = "{}",
                .message = failure_message,
            });
            protocol_failed = true;
            break;
        }
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
                                std::to_string(active_generation_.load()) +
                                ":" + std::to_string(stream_attempt),
                            .raw_payload = item.dump(),
                        });
                        const json listen = {
                            {"action", "listen"},
                            {"data", {{"streams", {"trade_updates"}}}},
                        };
                        SendText(socket, listen.dump());
                    } else {
                        events_.Push(OperationalEvent(
                            OperationalComponent::AccountStream,
                            OperationalState::Failed,
                            OperationalSeverity::Critical,
                            "Account stream authorization failed",
                            OperationalReason::AuthenticationFailure));
                        PublishCoreEvent(tradebox::core::BrokerEvent{
                            .kind = tradebox::core::BrokerEventKind::Failure,
                            .raw_payload = item.dump(),
                            .message =
                                "Account stream authorization failed",
                        });
                        protocol_failed = true;
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
                        reached_ready_state = true;
                        account_dirty_ = true;
                        positions_dirty_ = true;
                        orders_dirty_ = true;
                        activities_dirty_ = true;
                        PublishCoreEvent(tradebox::core::BrokerEvent{
                            .kind = tradebox::core::BrokerEventKind::
                                TradeUpdatesAcknowledged,
                            .source_event_id =
                                "listening:" +
                                std::to_string(active_generation_.load()) +
                                ":" + std::to_string(stream_attempt),
                            .raw_payload = item.dump(),
                        });
                        UiEvent event;
                        event.type = UiEventType::AccountStreamConnected;
                        event.operational_component =
                            OperationalComponent::AccountStream;
                        event.operational_state =
                            OperationalState::Subscribed;
                        event.operational_severity =
                            OperationalSeverity::Informational;
                        event.latency_ms =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - auth_started)
                                .count();
                        event.received_at_ms = WallClockNowMs();
                        events_.Push(std::move(event));
                    } else if (!trade_updates_active) {
                        events_.Push(OperationalEvent(
                            OperationalComponent::AccountStream,
                            OperationalState::Failed,
                            OperationalSeverity::Critical,
                            "Account trade_updates subscription missing",
                            OperationalReason::SubscriptionMismatch));
                        protocol_failed = true;
                    }
                } else if (stream == "trade_updates") {
                    orders_dirty_ = true;
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
                    const bool activity_relevant =
                        update.event == "fill" ||
                        update.event == "partial_fill" ||
                        update.event == "trade_correct" ||
                        update.event == "trade_bust";
                    PublishCoreEvent(tradebox::core::BrokerEvent{
                        .kind =
                            tradebox::core::BrokerEventKind::TradeUpdate,
                        .source_event_id = std::move(source_event_id),
                        .raw_payload = item.dump(),
                        .payload = std::move(update),
                    });
                    if (activity_relevant) activities_dirty_ = true;
                    if (activity_relevant) {
                        account_dirty_ = true;
                        positions_dirty_ = true;
                    }
                    UiEvent event;
                    event.type = UiEventType::AccountStreamEvent;
                    event.received_at_ms = received_at;
                    event.latency_ms =
                        timestamp > 0 ? received_at - timestamp : -1;
                    events_.Push(std::move(event));
                }
            }
            if (protocol_failed) break;
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
        events_.Push(OperationalEvent(
            OperationalComponent::AccountStream,
            OperationalState::Disconnected,
            OperationalSeverity::Critical,
            "Account stream disconnected",
            receive_error == ERROR_WINHTTP_TIMEOUT
                ? OperationalReason::SilenceTimeout
                : OperationalReason::UnexpectedDisconnect));
        PublishCoreEvent(tradebox::core::BrokerEvent{
            .kind = tradebox::core::BrokerEventKind::Disconnected,
            .raw_payload = "{}",
            .message = "Account stream disconnected",
        });
    }
    return reached_ready_state;
}
