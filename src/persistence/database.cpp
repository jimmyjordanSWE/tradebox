#include "tradebox/persistence/database.h"

#include <windows.h>
#include <shlobj.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <ranges>
#include <system_error>
#include <type_traits>

namespace {

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void BindStaticText(sqlite3_stmt* statement, int column,
                    const std::string& value) {
    sqlite3_bind_text64(
        statement, column, value.data(),
        static_cast<sqlite3_uint64>(value.size()),
        SQLITE_STATIC, SQLITE_UTF8);
}

void BindStaticText(sqlite3_stmt* statement, int column,
                    std::string_view value) {
    sqlite3_bind_text64(
        statement, column, value.data(),
        static_cast<sqlite3_uint64>(value.size()),
        SQLITE_STATIC, SQLITE_UTF8);
}

void BindNullableStaticText(
    sqlite3_stmt* statement, int column,
    const std::string* value) {
    if (!value || value->empty())
        sqlite3_bind_null(statement, column);
    else
        BindStaticText(statement, column, *value);
}

std::string FeedName(tradebox::core::MarketDataFeed feed) {
    switch (feed) {
        case tradebox::core::MarketDataFeed::Sip:
            return "sip";
        case tradebox::core::MarketDataFeed::Iex:
            return "iex";
        default:
            return "unknown";
    }
}

struct MarketEventMetadata {
    std::string_view kind;
    std::string_view instrument_id;
    std::string_view symbol;
    std::int64_t event_time_ns = 0;
    std::int64_t received_at_ms = 0;
};

MarketEventMetadata Metadata(
    const tradebox::core::MarketDataEvent& event) {
    return std::visit(
        [](const auto& typed) -> MarketEventMetadata {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<
                              T, tradebox::core::QuoteReceived>) {
                return {"q", typed.quote.instrument_id,
                        typed.quote.symbol,
                        typed.quote.event_time_ns,
                        typed.quote.received_at_ms};
            } else if constexpr (std::is_same_v<
                                     T,
                                     tradebox::core::TradeReceived>) {
                return {"t", typed.trade.instrument_id,
                        typed.trade.symbol,
                        typed.trade.event_time_ns,
                        typed.trade.received_at_ms};
            } else if constexpr (std::is_same_v<
                                     T,
                                     tradebox::core::TradeCanceled>) {
                return {"x", typed.instrument_id, typed.symbol,
                        typed.event_time_ns,
                        typed.received_at_ms};
            } else if constexpr (std::is_same_v<
                                     T,
                                     tradebox::core::TradeCorrected>) {
                return {"c", typed.instrument_id, typed.symbol,
                        typed.corrected_trade.event_time_ns,
                        typed.corrected_trade.received_at_ms};
            } else {
                return {};
            }
        },
        event);
}

std::string EncodeConditions(
    const std::vector<std::string>& conditions) {
    std::string result;
    for (const std::string& condition : conditions) {
        result += std::to_string(condition.size());
        result.push_back(':');
        result += condition;
    }
    return result;
}

std::vector<std::string> DecodeConditions(
    std::string_view encoded) {
    std::vector<std::string> result;
    std::size_t cursor = 0;
    while (cursor < encoded.size()) {
        const std::size_t colon = encoded.find(':', cursor);
        if (colon == std::string_view::npos) break;
        std::size_t length = 0;
        try {
            length = static_cast<std::size_t>(
                std::stoull(std::string(
                    encoded.substr(cursor, colon - cursor))));
        } catch (...) {
            break;
        }
        cursor = colon + 1;
        if (length > encoded.size() - cursor) break;
        result.emplace_back(encoded.substr(cursor, length));
        cursor += length;
    }
    return result;
}

constexpr std::int64_t kDayNs =
    24LL * 60 * 60 * 1'000'000'000;

std::string TickTradeKey(std::string_view trade_id,
                         std::int64_t event_time_ns) {
    return std::string(trade_id) + "@" +
           std::to_string(event_time_ns / kDayNs);
}

std::vector<std::string> JsonStrings(
    const nlohmann::json& value, const char* key) {
    std::vector<std::string> result;
    if (!value.contains(key) || !value[key].is_array()) return result;
    for (const auto& item : value[key])
        if (item.is_string()) result.push_back(item.get<std::string>());
    return result;
}

std::string JsonString(
    const nlohmann::json& value, const char* key) {
    if (!value.contains(key) || !value[key].is_string()) return {};
    return value[key].get<std::string>();
}

std::string JsonIdentifier(
    const nlohmann::json& value, const char* key) {
    if (!value.contains(key) || value[key].is_null()) return {};
    if (value[key].is_string()) return value[key].get<std::string>();
    if (value[key].is_number_integer())
        return std::to_string(value[key].get<std::int64_t>());
    if (value[key].is_number_unsigned())
        return std::to_string(value[key].get<std::uint64_t>());
    return {};
}

tradebox::core::Decimal JsonDecimal(
    const nlohmann::json& value, const char* key) {
    if (!value.contains(key) || value[key].is_null())
        return tradebox::core::Decimal::Zero();
    const std::string text =
        value[key].is_string() ? value[key].get<std::string>()
                               : value[key].dump();
    const auto parsed = tradebox::core::Decimal::Parse(text);
    return parsed ? *parsed : tradebox::core::Decimal::Zero();
}

nlohmann::json CommandContextJson(
    const tradebox::core::OrderCommandContext& context) {
    return {
        {"request_id", context.request_id},
        {"source", context.source},
        {"account_id", context.account_id},
        {"environment", static_cast<int>(context.environment)},
        {"generation", context.generation.value},
        {"live_trading_confirmed", context.live_trading_confirmed},
    };
}

void OptionalDecimalJson(
    nlohmann::json& value, const char* key,
    const std::optional<tradebox::core::Decimal>& decimal) {
    if (decimal) value[key] = decimal->ToString();
}

nlohmann::json NativeOrderJson(
    const tradebox::core::NativeOrderRequest& order) {
    nlohmann::json value{
        {"asset_class", static_cast<int>(order.asset_class)},
        {"symbol", order.symbol},
        {"type", static_cast<int>(order.type)},
        {"time_in_force", static_cast<int>(order.time_in_force)},
        {"order_class", static_cast<int>(order.order_class)},
        {"extended_hours", order.extended_hours},
        {"client_order_id", order.client_order_id},
    };
    OptionalDecimalJson(value, "qty", order.qty);
    OptionalDecimalJson(value, "notional", order.notional);
    if (order.side) value["side"] = static_cast<int>(*order.side);
    if (order.position_intent)
        value["position_intent"] =
            static_cast<int>(*order.position_intent);
    OptionalDecimalJson(value, "limit_price", order.limit_price);
    OptionalDecimalJson(value, "stop_price", order.stop_price);
    OptionalDecimalJson(value, "trail_price", order.trail_price);
    OptionalDecimalJson(value, "trail_percent", order.trail_percent);
    if (order.take_profit)
        value["take_profit"] = {
            {"limit_price",
             order.take_profit->limit_price.ToString()},
        };
    if (order.stop_loss) {
        nlohmann::json stop{
            {"stop_price", order.stop_loss->stop_price.ToString()},
        };
        OptionalDecimalJson(stop, "limit_price",
                            order.stop_loss->limit_price);
        value["stop_loss"] = std::move(stop);
    }
    value["legs"] = nlohmann::json::array();
    for (const tradebox::core::OrderLeg& leg : order.legs) {
        value["legs"].push_back({
            {"symbol", leg.symbol},
            {"ratio_qty", leg.ratio_qty.ToString()},
            {"side", static_cast<int>(leg.side)},
            {"position_intent",
             static_cast<int>(leg.position_intent)},
        });
    }
    return value;
}

nlohmann::json ReplacementJson(
    const tradebox::core::ReplaceOrderRequest& replacement) {
    nlohmann::json value;
    OptionalDecimalJson(value, "qty", replacement.qty);
    OptionalDecimalJson(value, "notional", replacement.notional);
    if (replacement.time_in_force)
        value["time_in_force"] =
            static_cast<int>(*replacement.time_in_force);
    OptionalDecimalJson(value, "limit_price",
                        replacement.limit_price);
    OptionalDecimalJson(value, "stop_price",
                        replacement.stop_price);
    OptionalDecimalJson(value, "trail", replacement.trail);
    if (replacement.client_order_id)
        value["client_order_id"] = *replacement.client_order_id;
    return value;
}

std::string SerializeCommand(
    const tradebox::core::NativeOrderCommand& command) {
    return std::visit(
               [](const auto& typed) {
                   using T = std::decay_t<decltype(typed)>;
                   nlohmann::json value{
                       {"context", CommandContextJson(typed.context)},
                   };
                   if constexpr (std::is_same_v<
                                     T,
                                     tradebox::core::PlaceOrderCommand>) {
                       value["kind"] = "place";
                       value["order"] = NativeOrderJson(typed.order);
                   } else if constexpr (std::is_same_v<
                                            T,
                                            tradebox::core::CancelOrderCommand>) {
                       value["kind"] = "cancel";
                       value["order_id"] = typed.order_id;
                   } else if constexpr (std::is_same_v<
                                            T,
                                            tradebox::core::ReplaceOrderCommand>) {
                       value["kind"] = "replace";
                       value["order_id"] = typed.order_id;
                       value["replacement"] =
                           ReplacementJson(typed.replacement);
                   } else if constexpr (std::is_same_v<
                                            T,
                                            tradebox::core::ClosePositionCommand>) {
                       value["kind"] = "close_position";
                       value["symbol_or_asset_id"] =
                           typed.symbol_or_asset_id;
                       if (typed.qty)
                           value["qty"] = typed.qty->ToString();
                       if (typed.percentage)
                           value["percentage"] =
                               typed.percentage->ToString();
                   } else if constexpr (std::is_same_v<
                                            T,
                                            tradebox::core::CloseAllPositionsCommand>) {
                       value["kind"] = "close_all_positions";
                       value["cancel_open_orders"] =
                           typed.cancel_open_orders;
                   } else {
                       value["kind"] = "cancel_all_orders";
                   }
                   return value;
               },
               command)
        .dump();
}

std::string SerializeCommandItems(
    const std::vector<tradebox::core::CommandItemResult>& items) {
    nlohmann::json value = nlohmann::json::array();
    for (const auto& item : items) {
        value.push_back({
            {"id", item.id},
            {"symbol", item.symbol},
            {"http_status", item.http_status},
            {"accepted", item.accepted},
            {"message", item.message},
            {"raw_response", item.raw_response},
        });
    }
    return value.dump();
}

std::vector<tradebox::core::CommandItemResult> ParseCommandItems(
    const std::string& text) {
    std::vector<tradebox::core::CommandItemResult> items;
    if (text.empty()) return items;
    try {
        const auto value = nlohmann::json::parse(text);
        if (!value.is_array()) return items;
        for (const auto& entry : value) {
            if (!entry.is_object()) continue;
            items.push_back({
                .id = entry.value("id", ""),
                .symbol = entry.value("symbol", ""),
                .http_status = entry.value("http_status", 0U),
                .accepted = entry.value("accepted", false),
                .message = entry.value("message", ""),
                .raw_response = entry.value("raw_response", ""),
            });
        }
    } catch (const std::exception&) {
    }
    return items;
}

}  // namespace

Database::Database() = default;

Database::~Database() {
    {
        std::scoped_lock lock(queue_mutex_);
        stopping_ = true;
    }
    queue_cv_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (market_db_) sqlite3_close(market_db_);
    if (db_) sqlite3_close(db_);
}

bool Database::Open(std::string& error) {
    PWSTR local_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                    &local_path))) {
        error = "Could not locate Local AppData";
        return false;
    }
    data_directory_ = std::filesystem::path(local_path) / L"TradeBox";
    CoTaskMemFree(local_path);
    std::error_code ec;
    std::filesystem::create_directories(data_directory_, ec);
    if (ec) {
        error = "Could not create data directory: " + ec.message();
        return false;
    }
    return OpenAt(data_directory_ / L"tradebox_app.db", error);
}

bool Database::OpenAt(const std::filesystem::path& database_path,
                      std::string& error) {
    if (db_) {
        error = "Database is already open";
        return false;
    }
    data_directory_ = database_path.parent_path();
    std::error_code ec;
    if (!data_directory_.empty())
        std::filesystem::create_directories(data_directory_, ec);
    if (ec) {
        error = "Could not create data directory: " + ec.message();
        return false;
    }
    // Every access to either connection is serialized by db_mutex_.
    // NOMUTEX removes SQLite's duplicate per-connection lock; it does
    // not permit concurrent use of a connection.
    const int rc = sqlite3_open_v2(
        database_path.string().c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
            SQLITE_OPEN_NOMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        error = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_busy_timeout(db_, 3000);
    const char* schema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
CREATE TABLE IF NOT EXISTS daily_bars (
  symbol TEXT NOT NULL,
  timestamp_ms INTEGER NOT NULL,
  open REAL NOT NULL,
  high REAL NOT NULL,
  low REAL NOT NULL,
  close REAL NOT NULL,
  volume REAL NOT NULL,
  feed TEXT NOT NULL DEFAULT 'iex',
  PRIMARY KEY(symbol, timestamp_ms, feed)
);
CREATE TABLE IF NOT EXISTS market_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  received_at_ms INTEGER NOT NULL,
  event_time_ms INTEGER,
  kind TEXT NOT NULL,
  symbol TEXT,
  payload TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS market_events_time
  ON market_events(event_time_ms, symbol);
CREATE TABLE IF NOT EXISTS timeline_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  source TEXT NOT NULL,
  source_event_id TEXT,
  kind TEXT NOT NULL,
  symbol TEXT,
  event_time_ms INTEGER,
  available_at_ms INTEGER NOT NULL,
  recorded_at_ms INTEGER NOT NULL,
  payload_json TEXT NOT NULL,
  schema_version INTEGER NOT NULL DEFAULT 1,
  UNIQUE(source, source_event_id)
);
CREATE INDEX IF NOT EXISTS timeline_events_replay
  ON timeline_events(available_at_ms, id);
CREATE INDEX IF NOT EXISTS timeline_events_symbol_time
  ON timeline_events(symbol, event_time_ms);
CREATE TABLE IF NOT EXISTS core_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  source_event_id TEXT,
  kind TEXT NOT NULL,
  connection_generation INTEGER NOT NULL,
  received_at_ms INTEGER NOT NULL,
  recorded_at_ms INTEGER NOT NULL,
  payload_json TEXT NOT NULL,
  schema_version INTEGER NOT NULL DEFAULT 1,
  UNIQUE(source_event_id)
);
CREATE INDEX IF NOT EXISTS core_events_generation
  ON core_events(connection_generation, id);
CREATE TABLE IF NOT EXISTS order_commands (
  request_id TEXT PRIMARY KEY,
  source TEXT NOT NULL,
  command_kind INTEGER NOT NULL,
  account_id TEXT NOT NULL,
  environment INTEGER NOT NULL,
  connection_generation INTEGER NOT NULL,
  client_order_id TEXT,
  created_at_ms INTEGER NOT NULL,
  dispatch_started_at_ms INTEGER,
  completed_at_ms INTEGER,
  outcome INTEGER,
  broker_order_id TEXT,
  http_status INTEGER,
  message TEXT,
  raw_response TEXT,
  items_json TEXT NOT NULL DEFAULT '[]',
  reconciliation_required INTEGER NOT NULL DEFAULT 0,
  recovery_state INTEGER NOT NULL DEFAULT 0,
  recovery_message TEXT NOT NULL DEFAULT '',
  payload_json TEXT NOT NULL,
  schema_version INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX IF NOT EXISTS order_commands_account_time
  ON order_commands(account_id, created_at_ms);
CREATE TABLE IF NOT EXISTS account_activities (
  account_id TEXT NOT NULL,
  provider_id TEXT NOT NULL,
  activity_type TEXT NOT NULL,
  activity_subtype TEXT NOT NULL,
  execution_type TEXT NOT NULL,
  status TEXT NOT NULL,
  symbol TEXT NOT NULL,
  cusip TEXT NOT NULL,
  side TEXT NOT NULL,
  order_id TEXT NOT NULL,
  currency TEXT NOT NULL,
  occurred_at TEXT NOT NULL,
  settlement_date TEXT NOT NULL,
  occurred_at_ms INTEGER NOT NULL,
  qty_text TEXT,
  price_text TEXT,
  cumulative_qty_text TEXT,
  leaves_qty_text TEXT,
  net_amount_text TEXT,
  per_share_amount_text TEXT,
  fill_reconciliation INTEGER NOT NULL,
  raw_payload TEXT NOT NULL,
  revision INTEGER NOT NULL DEFAULT 1,
  recorded_at_ms INTEGER NOT NULL,
  PRIMARY KEY(account_id, provider_id)
);
CREATE INDEX IF NOT EXISTS account_activities_time
  ON account_activities(account_id, occurred_at_ms DESC, provider_id DESC);
CREATE INDEX IF NOT EXISTS account_activities_order
  ON account_activities(account_id, order_id);
CREATE INDEX IF NOT EXISTS account_activities_symbol
  ON account_activities(account_id, symbol, occurred_at_ms DESC);
CREATE TABLE IF NOT EXISTS tags (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  namespace TEXT NOT NULL DEFAULT 'user',
  name TEXT NOT NULL,
  description TEXT,
  UNIQUE(namespace, name)
);
CREATE TABLE IF NOT EXISTS timeline_event_tags (
  event_id INTEGER NOT NULL REFERENCES timeline_events(id),
  tag_id INTEGER NOT NULL REFERENCES tags(id),
  assigned_at_ms INTEGER NOT NULL,
  assigned_by TEXT NOT NULL,
  confidence REAL,
  metadata_json TEXT,
  PRIMARY KEY(event_id, tag_id, assigned_by)
);
INSERT OR IGNORE INTO timeline_events(
  source, source_event_id, kind, symbol, event_time_ms, available_at_ms,
  recorded_at_ms, payload_json, schema_version
)
SELECT
  'alpaca.market.iex', 'legacy:' || id, kind, symbol, event_time_ms,
  received_at_ms, received_at_ms, payload, 1
FROM market_events;
)SQL";
    if (!Execute(schema, &error)) return false;
    // UI/application state belongs to workstation profiles now. Remove the
    // legacy tables from existing operational databases without touching
    // broker journals or market history.
    Execute("DROP TABLE IF EXISTS watchlist; DROP TABLE IF EXISTS "
            "window_placement; DROP TABLE IF EXISTS account_aliases; "
            "DROP TABLE IF EXISTS app_settings;",
            nullptr);
    Execute(
        "ALTER TABLE order_commands ADD COLUMN items_json TEXT NOT NULL "
        "DEFAULT '[]'",
        nullptr);
    Execute(
        "ALTER TABLE order_commands ADD COLUMN reconciliation_required "
        "INTEGER NOT NULL DEFAULT 0",
        nullptr);
    Execute(
        "ALTER TABLE order_commands ADD COLUMN dispatch_started_at_ms "
        "INTEGER",
        nullptr);
    Execute(
        "ALTER TABLE order_commands ADD COLUMN recovery_state "
        "INTEGER NOT NULL DEFAULT 0",
        nullptr);
    Execute(
        "ALTER TABLE order_commands ADD COLUMN recovery_message "
        "TEXT NOT NULL DEFAULT ''",
        nullptr);
    const int market_rc = sqlite3_open_v2(
        (data_directory_ / L"market_data.db").string().c_str(), &market_db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
            SQLITE_OPEN_NOMUTEX,
        nullptr);
    if (market_rc != SQLITE_OK) {
        error = market_db_ ? sqlite3_errmsg(market_db_) : "Could not open market data database";
        return false;
    }
    sqlite3_busy_timeout(market_db_, 3000);
    const char* market_schema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
CREATE TABLE IF NOT EXISTS daily_bars (
  symbol TEXT NOT NULL,
  timestamp_ms INTEGER NOT NULL,
  open REAL NOT NULL,
  high REAL NOT NULL,
  low REAL NOT NULL,
  close REAL NOT NULL,
  volume REAL NOT NULL,
  feed TEXT NOT NULL DEFAULT 'iex',
  timeframe TEXT NOT NULL DEFAULT '1Day',
  PRIMARY KEY(symbol, timestamp_ms, feed, timeframe)
);
CREATE TABLE IF NOT EXISTS market_metadata (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS market_tick_events (
  id INTEGER PRIMARY KEY,
  feed TEXT NOT NULL,
  source_event_id TEXT,
  kind TEXT NOT NULL,
  instrument_id TEXT NOT NULL DEFAULT '',
  symbol TEXT NOT NULL,
  event_time_ns INTEGER NOT NULL,
  received_at_ms INTEGER NOT NULL,
  raw_payload TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS typed_market_ticks (
  id INTEGER PRIMARY KEY,
  feed TEXT NOT NULL,
  source_event_id TEXT,
  kind TEXT NOT NULL,
  instrument_id TEXT NOT NULL DEFAULT '',
  symbol TEXT NOT NULL,
  event_time_ns INTEGER NOT NULL,
  received_at_ms INTEGER NOT NULL,
  broker_timestamp TEXT,
  trade_id TEXT,
  original_trade_id TEXT,
  price_text TEXT,
  size_text TEXT,
  exchange TEXT,
  conditions_text TEXT,
  tape TEXT,
  bid_price_text TEXT,
  bid_size_text TEXT,
  bid_exchange TEXT,
  ask_price_text TEXT,
  ask_size_text TEXT,
  ask_exchange TEXT,
  corrected INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS market_tick_coverage (
  symbol TEXT NOT NULL,
  feed TEXT NOT NULL,
  kind TEXT NOT NULL,
  start_ns INTEGER NOT NULL,
  end_ns INTEGER NOT NULL,
  completed_at_ms INTEGER NOT NULL,
  PRIMARY KEY(symbol, feed, kind, start_ns, end_ns)
);
CREATE TABLE IF NOT EXISTS market_tick_coverage_v2 (
  instrument_id TEXT NOT NULL,
  symbol TEXT NOT NULL,
  feed TEXT NOT NULL,
  kind TEXT NOT NULL,
  start_ns INTEGER NOT NULL,
  end_ns INTEGER NOT NULL,
  completed_at_ms INTEGER NOT NULL,
  PRIMARY KEY(instrument_id, feed, kind, start_ns, end_ns)
);
CREATE TABLE IF NOT EXISTS provider_bars (
  instrument_id TEXT NOT NULL,
  symbol TEXT NOT NULL,
  feed INTEGER NOT NULL,
  timeframe TEXT NOT NULL,
  adjustment INTEGER NOT NULL,
  start_ns INTEGER NOT NULL,
  open_text TEXT NOT NULL,
  high_text TEXT NOT NULL,
  low_text TEXT NOT NULL,
  close_text TEXT NOT NULL,
  volume_text TEXT NOT NULL,
  vwap_text TEXT,
  trade_count INTEGER NOT NULL,
  source INTEGER NOT NULL,
  state INTEGER NOT NULL,
  revision INTEGER NOT NULL,
  PRIMARY KEY(instrument_id,feed,timeframe,adjustment,start_ns)
);
CREATE TABLE IF NOT EXISTS provider_bar_coverage (
  instrument_id TEXT NOT NULL,
  feed INTEGER NOT NULL,
  timeframe TEXT NOT NULL,
  adjustment INTEGER NOT NULL,
  start_ns INTEGER NOT NULL,
  end_ns INTEGER NOT NULL,
  PRIMARY KEY(instrument_id,feed,timeframe,adjustment,start_ns,end_ns)
);
)SQL";
    if (!ExecuteMarket(market_schema, &error)) return false;
    // The market database can contain millions of ticks. These index changes
    // are a one-time migration; dropping and rebuilding them on every launch
    // makes startup scan the entire database repeatedly.
    int market_schema_version = 0;
    sqlite3_stmt* version_statement = nullptr;
    if (sqlite3_prepare_v2(
            market_db_, "PRAGMA user_version", -1,
            &version_statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(market_db_);
        return false;
    }
    if (sqlite3_step(version_statement) == SQLITE_ROW)
        market_schema_version = sqlite3_column_int(version_statement, 0);
    sqlite3_finalize(version_statement);
    if (market_schema_version < 1) {
        ExecuteMarket(
            "ALTER TABLE market_tick_events ADD COLUMN instrument_id "
            "TEXT NOT NULL DEFAULT ''",
            nullptr);
        ExecuteMarket(
            "DROP INDEX IF EXISTS market_tick_events_series;"
            "CREATE INDEX IF NOT EXISTS market_tick_events_series "
            "ON market_tick_events("
            "instrument_id,feed,event_time_ns);"
            "CREATE UNIQUE INDEX IF NOT EXISTS market_tick_events_source "
            "ON market_tick_events(feed,source_event_id) "
            "WHERE source_event_id IS NOT NULL;"
            "CREATE INDEX IF NOT EXISTS market_tick_events_corrections "
            "ON market_tick_events(instrument_id,feed,event_time_ns) "
            "WHERE kind IN ('x','c');"
            "DROP INDEX IF EXISTS market_tick_coverage_lookup;"
            "DROP INDEX IF EXISTS market_tick_coverage_v2_lookup;"
            "DROP INDEX IF EXISTS provider_bars_range;",
            nullptr);
        ExecuteMarket(
            "DROP INDEX IF EXISTS typed_market_ticks_series;"
            "CREATE INDEX IF NOT EXISTS typed_market_ticks_series "
            "ON typed_market_ticks("
            "instrument_id,feed,event_time_ns);"
            "CREATE UNIQUE INDEX IF NOT EXISTS typed_market_ticks_source "
            "ON typed_market_ticks(feed,source_event_id) "
            "WHERE source_event_id IS NOT NULL;"
            "CREATE INDEX IF NOT EXISTS typed_market_ticks_corrections "
            "ON typed_market_ticks(instrument_id,feed,event_time_ns) "
            "WHERE kind IN ('x','c');",
            nullptr);
        if (!ExecuteMarket("PRAGMA user_version = 1", &error)) return false;
    }
    writer_ = std::thread(&Database::WriterLoop, this);
    return true;
}

bool Database::Execute(const char* sql, std::string* error) {
    char* sqlite_error = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &sqlite_error);
    if (rc != SQLITE_OK) {
        if (error) *error = sqlite_error ? sqlite_error : "SQLite error";
        sqlite3_free(sqlite_error);
        return false;
    }
    return true;
}

bool Database::ExecuteMarket(const char* sql, std::string* error) {
    char* sqlite_error = nullptr;
    const int rc = sqlite3_exec(market_db_, sql, nullptr, nullptr, &sqlite_error);
    if (rc != SQLITE_OK) {
        if (error) *error = sqlite_error ? sqlite_error : "SQLite market data error";
        sqlite3_free(sqlite_error);
        return false;
    }
    return true;
}

bool Database::AppendCoreEvent(std::string source_event_id, std::string kind,
                               std::uint64_t generation,
                               std::int64_t received_at_ms,
                               std::string payload, std::string& error) {
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT OR IGNORE INTO core_events("
        "source_event_id,kind,connection_generation,received_at_ms,"
        "recorded_at_ms,payload_json,schema_version"
        ") VALUES(?,?,?,?,?,?,1)";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db_);
        return false;
    }
    if (source_event_id.empty())
        sqlite3_bind_null(statement, 1);
    else
        sqlite3_bind_text(statement, 1, source_event_id.c_str(), -1,
                          SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3,
                       static_cast<sqlite3_int64>(generation));
    sqlite3_bind_int64(statement, 4, received_at_ms);
    sqlite3_bind_int64(statement, 5, NowMs());
    sqlite3_bind_text(statement, 6, payload.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(statement);
    if (rc != SQLITE_DONE) error = sqlite3_errmsg(db_);
    sqlite3_finalize(statement);
    return rc == SQLITE_DONE;
}

std::expected<tradebox::core::ReservationResult, std::string>
Database::ReserveOrderCommand(
    const tradebox::core::OrderCommandRecord& record,
    const tradebox::core::NativeOrderCommand& command) {
    const std::string payload = SerializeCommand(command);
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    const char* insert_sql =
        "INSERT INTO order_commands("
        "request_id,source,command_kind,account_id,environment,"
        "connection_generation,client_order_id,created_at_ms,payload_json,"
        "schema_version"
        ") VALUES(?,?,?,?,?,?,?,?,?,1)";
    if (sqlite3_prepare_v2(db_, insert_sql, -1, &statement, nullptr) !=
        SQLITE_OK)
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    sqlite3_bind_text(statement, 1, record.request_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, record.source.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 3,
                     static_cast<int>(record.kind));
    sqlite3_bind_text(statement, 4, record.account_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 5,
                     static_cast<int>(record.environment));
    sqlite3_bind_int64(
        statement, 6,
        static_cast<sqlite3_int64>(record.generation.value));
    if (record.client_order_id.empty())
        sqlite3_bind_null(statement, 7);
    else
        sqlite3_bind_text(statement, 7, record.client_order_id.c_str(), -1,
                          SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 8, record.created_at_ms);
    sqlite3_bind_text(statement, 9, payload.c_str(), -1,
                      SQLITE_TRANSIENT);
    const int insert_result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (insert_result == SQLITE_DONE)
        return tradebox::core::ReservationResult{
            .reservation =
                tradebox::core::CommandReservation::Reserved,
        };
    if (insert_result != SQLITE_CONSTRAINT)
        return std::unexpected(std::string(sqlite3_errmsg(db_)));

    const char* select_sql =
        "SELECT outcome,broker_order_id,http_status,message,raw_response,"
        "items_json,reconciliation_required,recovery_state,recovery_message "
        "FROM order_commands WHERE request_id=?";
    if (sqlite3_prepare_v2(db_, select_sql, -1, &statement, nullptr) !=
        SQLITE_OK)
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    sqlite3_bind_text(statement, 1, record.request_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    tradebox::core::ReservationResult duplicate{
        .reservation = tradebox::core::CommandReservation::Duplicate,
    };
    if (sqlite3_step(statement) == SQLITE_ROW &&
        sqlite3_column_type(statement, 0) != SQLITE_NULL) {
        tradebox::core::OrderCommandResult existing;
        existing.request_id = record.request_id;
        existing.outcome =
            static_cast<tradebox::core::OrderCommandOutcome>(
                sqlite3_column_int(statement, 0));
        const auto text = [statement](int column) {
            const unsigned char* value =
                sqlite3_column_text(statement, column);
            return value ? std::string(
                               reinterpret_cast<const char*>(value))
                         : std::string{};
        };
        existing.broker_order_id = text(1);
        existing.http_status = static_cast<std::uint32_t>(
            sqlite3_column_int(statement, 2));
        existing.message = text(3);
        existing.raw_response = text(4);
        existing.items = ParseCommandItems(text(5));
        existing.reconciliation_required =
            sqlite3_column_int(statement, 6) != 0;
        existing.recovery_state =
            static_cast<tradebox::core::CommandRecoveryState>(
                sqlite3_column_int(statement, 7));
        existing.recovery_message = text(8);
        duplicate.existing_result = std::move(existing);
    }
    sqlite3_finalize(statement);
    return duplicate;
}

std::expected<void, std::string> Database::CompleteOrderCommand(
    const tradebox::core::OrderCommandResult& result) {
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "UPDATE order_commands SET completed_at_ms=?,outcome=?,"
        "broker_order_id=?,http_status=?,message=?,raw_response=?,"
        "items_json=?,reconciliation_required=?,recovery_state=?,"
        "recovery_message=? "
        "WHERE request_id=? AND outcome IS NULL";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) !=
        SQLITE_OK)
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    sqlite3_bind_int64(statement, 1, NowMs());
    sqlite3_bind_int(statement, 2, static_cast<int>(result.outcome));
    sqlite3_bind_text(statement, 3, result.broker_order_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4,
                     static_cast<int>(result.http_status));
    sqlite3_bind_text(statement, 5, result.message.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, result.raw_response.c_str(), -1,
                      SQLITE_TRANSIENT);
    const std::string items = SerializeCommandItems(result.items);
    sqlite3_bind_text(statement, 7, items.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 8,
                     result.reconciliation_required ? 1 : 0);
    sqlite3_bind_int(
        statement, 9, static_cast<int>(result.recovery_state));
    sqlite3_bind_text(
        statement, 10, result.recovery_message.c_str(), -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 11, result.request_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    const int rc = sqlite3_step(statement);
    const int changed = sqlite3_changes(db_);
    const std::string error =
        rc == SQLITE_DONE ? std::string{} : sqlite3_errmsg(db_);
    sqlite3_finalize(statement);
    if (rc != SQLITE_DONE) return std::unexpected(error);
    if (changed != 1)
        return std::unexpected(
            "Order command completion did not update one reserved command");
    return {};
}

std::expected<void, std::string>
Database::MarkOrderCommandDispatchStarted(
    const std::string& request_id) {
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "UPDATE order_commands SET dispatch_started_at_ms=? "
        "WHERE request_id=? AND outcome IS NULL "
        "AND dispatch_started_at_ms IS NULL";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) !=
        SQLITE_OK)
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    sqlite3_bind_int64(statement, 1, NowMs());
    sqlite3_bind_text(statement, 2, request_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    const int rc = sqlite3_step(statement);
    const int changed = sqlite3_changes(db_);
    const std::string error =
        rc == SQLITE_DONE ? std::string{} : sqlite3_errmsg(db_);
    sqlite3_finalize(statement);
    if (rc != SQLITE_DONE) return std::unexpected(error);
    if (changed != 1)
        return std::unexpected(
            "Order command dispatch marker did not update one "
            "reserved command");
    return {};
}

std::expected<
    std::vector<tradebox::core::RecoverableOrderCommand>,
    std::string>
Database::LoadRecoverableOrderCommands() {
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT request_id,source,command_kind,account_id,environment,"
        "connection_generation,client_order_id,created_at_ms,"
        "dispatch_started_at_ms,outcome,recovery_state,recovery_message,"
        "payload_json FROM order_commands "
        "WHERE outcome IS NULL OR recovery_state=? OR "
        "(outcome=? AND recovery_state=0)";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) !=
        SQLITE_OK)
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    sqlite3_bind_int(
        statement, 1,
        static_cast<int>(
            tradebox::core::CommandRecoveryState::Pending));
    sqlite3_bind_int(
        statement, 2,
        static_cast<int>(
            tradebox::core::OrderCommandOutcome::Indeterminate));
    std::vector<tradebox::core::RecoverableOrderCommand> result;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto text = [statement](int column) {
            const unsigned char* value =
                sqlite3_column_text(statement, column);
            return value
                       ? std::string(
                             reinterpret_cast<const char*>(value))
                       : std::string{};
        };
        tradebox::core::RecoverableOrderCommand command{
            .record = {
                .request_id = text(0),
                .source = text(1),
                .kind =
                    static_cast<tradebox::core::OrderCommandKind>(
                        sqlite3_column_int(statement, 2)),
                .account_id = text(3),
                .environment =
                    static_cast<tradebox::core::AccountEnvironment>(
                        sqlite3_column_int(statement, 4)),
                .generation = tradebox::core::ConnectionGeneration{
                    static_cast<std::uint64_t>(
                        sqlite3_column_int64(statement, 5))},
                .client_order_id = text(6),
                .created_at_ms = sqlite3_column_int64(statement, 7),
            },
            .dispatch_started =
                sqlite3_column_type(statement, 8) != SQLITE_NULL ||
                sqlite3_column_type(statement, 9) != SQLITE_NULL,
            .recovery_state =
                static_cast<tradebox::core::CommandRecoveryState>(
                    sqlite3_column_int(statement, 10)),
            .recovery_message = text(11),
        };
        try {
            const nlohmann::json payload =
                nlohmann::json::parse(text(12));
            command.target_order_id =
                JsonString(payload, "order_id");
            command.symbol_or_asset_id =
                JsonString(payload, "symbol_or_asset_id");
            if (payload.contains("qty") &&
                !payload["qty"].is_null())
                command.qty = JsonDecimal(payload, "qty");
            if (payload.contains("percentage") &&
                !payload["percentage"].is_null())
                command.percentage =
                    JsonDecimal(payload, "percentage");
            command.cancel_open_orders =
                payload.value("cancel_open_orders", false);
        } catch (const std::exception& error) {
            sqlite3_finalize(statement);
            return std::unexpected(
                "Could not parse recoverable command " +
                command.record.request_id + ": " + error.what());
        }
        result.push_back(std::move(command));
    }
    sqlite3_finalize(statement);
    return result;
}

std::expected<void, std::string>
Database::ResolveOrderCommandRecovery(
    const tradebox::core::OrderCommandResult& result) {
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "UPDATE order_commands SET completed_at_ms=?,outcome=?,"
        "broker_order_id=?,http_status=?,message=?,raw_response=?,"
        "items_json=?,reconciliation_required=?,recovery_state=?,"
        "recovery_message=? WHERE request_id=? AND "
        "(outcome IS NULL OR recovery_state=? OR "
        "(outcome=? AND recovery_state=0))";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) !=
        SQLITE_OK)
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    sqlite3_bind_int64(statement, 1, NowMs());
    sqlite3_bind_int(statement, 2, static_cast<int>(result.outcome));
    sqlite3_bind_text(statement, 3, result.broker_order_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4,
                     static_cast<int>(result.http_status));
    sqlite3_bind_text(statement, 5, result.message.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, result.raw_response.c_str(), -1,
                      SQLITE_TRANSIENT);
    const std::string items = SerializeCommandItems(result.items);
    sqlite3_bind_text(statement, 7, items.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 8,
                     result.reconciliation_required ? 1 : 0);
    sqlite3_bind_int(
        statement, 9, static_cast<int>(result.recovery_state));
    sqlite3_bind_text(
        statement, 10, result.recovery_message.c_str(), -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 11, result.request_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(
        statement, 12,
        static_cast<int>(
            tradebox::core::CommandRecoveryState::Pending));
    sqlite3_bind_int(
        statement, 13,
        static_cast<int>(
            tradebox::core::OrderCommandOutcome::Indeterminate));
    const int rc = sqlite3_step(statement);
    const int changed = sqlite3_changes(db_);
    const std::string error =
        rc == SQLITE_DONE ? std::string{} : sqlite3_errmsg(db_);
    sqlite3_finalize(statement);
    if (rc != SQLITE_DONE) return std::unexpected(error);
    if (changed != 1)
        return std::unexpected(
            "Order command recovery did not update one recoverable "
            "command");
    return {};
}

std::expected<tradebox::core::OrderCommandLookup, std::string>
Database::LookupOrderCommand(const std::string& request_id) {
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT outcome,broker_order_id,http_status,message,raw_response,"
        "items_json,reconciliation_required,recovery_state,recovery_message "
        "FROM order_commands WHERE request_id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) !=
        SQLITE_OK)
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    sqlite3_bind_text(statement, 1, request_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    tradebox::core::OrderCommandLookup result;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        result.exists = true;
        const auto text = [statement](int column) {
            const unsigned char* value =
                sqlite3_column_text(statement, column);
            return value ? std::string(
                               reinterpret_cast<const char*>(value))
                         : std::string{};
        };
        if (sqlite3_column_type(statement, 0) != SQLITE_NULL) {
        result.terminal_result = tradebox::core::OrderCommandResult{
            .request_id = request_id,
            .outcome =
                static_cast<tradebox::core::OrderCommandOutcome>(
                    sqlite3_column_int(statement, 0)),
            .broker_order_id = text(1),
            .http_status = static_cast<std::uint32_t>(
                sqlite3_column_int(statement, 2)),
            .message = text(3),
            .raw_response = text(4),
            .items = ParseCommandItems(text(5)),
            .reconciliation_required =
                sqlite3_column_int(statement, 6) != 0,
            .recovery_state =
                static_cast<tradebox::core::CommandRecoveryState>(
                    sqlite3_column_int(statement, 7)),
            .recovery_message = text(8),
        };
        result.recovery_state =
            result.terminal_result->recovery_state;
        result.recovery_message =
            result.terminal_result->recovery_message;
        } else {
            result.recovery_state =
                static_cast<tradebox::core::CommandRecoveryState>(
                    sqlite3_column_int(statement, 7));
            result.recovery_message = text(8);
        }
    }
    sqlite3_finalize(statement);
    return result;
}

std::expected<tradebox::core::AccountActivityWriteResult, std::string>
Database::StoreAccountActivities(
    const std::vector<tradebox::core::AccountActivity>& activities) {
    tradebox::core::AccountActivityWriteResult result;
    if (activities.empty()) return result;
    std::scoped_lock lock(db_mutex_);
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) !=
        SQLITE_OK)
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    sqlite3_stmt* existing = nullptr;
    sqlite3_stmt* upsert = nullptr;
    const char* select_sql =
        "SELECT raw_payload,fill_reconciliation FROM account_activities "
        "WHERE account_id=? AND provider_id=?";
    const char* upsert_sql =
        "INSERT INTO account_activities("
        "account_id,provider_id,activity_type,activity_subtype,"
        "execution_type,status,symbol,cusip,side,order_id,currency,"
        "occurred_at,settlement_date,occurred_at_ms,qty_text,price_text,"
        "cumulative_qty_text,leaves_qty_text,net_amount_text,"
        "per_share_amount_text,fill_reconciliation,raw_payload,revision,"
        "recorded_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?) "
        "ON CONFLICT(account_id,provider_id) DO UPDATE SET "
        "activity_type=excluded.activity_type,"
        "activity_subtype=excluded.activity_subtype,"
        "execution_type=excluded.execution_type,status=excluded.status,"
        "symbol=excluded.symbol,cusip=excluded.cusip,side=excluded.side,"
        "order_id=excluded.order_id,currency=excluded.currency,"
        "occurred_at=excluded.occurred_at,"
        "settlement_date=excluded.settlement_date,"
        "occurred_at_ms=excluded.occurred_at_ms,qty_text=excluded.qty_text,"
        "price_text=excluded.price_text,"
        "cumulative_qty_text=excluded.cumulative_qty_text,"
        "leaves_qty_text=excluded.leaves_qty_text,"
        "net_amount_text=excluded.net_amount_text,"
        "per_share_amount_text=excluded.per_share_amount_text,"
        "fill_reconciliation=excluded.fill_reconciliation,"
        "raw_payload=excluded.raw_payload,"
        "revision=account_activities.revision + "
        "CASE WHEN account_activities.raw_payload<>excluded.raw_payload "
        "THEN 1 ELSE 0 END,recorded_at_ms=excluded.recorded_at_ms";
    if (sqlite3_prepare_v2(db_, select_sql, -1, &existing, nullptr) !=
            SQLITE_OK ||
        sqlite3_prepare_v2(db_, upsert_sql, -1, &upsert, nullptr) !=
            SQLITE_OK) {
        if (existing) sqlite3_finalize(existing);
        if (upsert) sqlite3_finalize(upsert);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    }
    const auto bind_text = [](sqlite3_stmt* statement, int column,
                              const std::string& text) {
        sqlite3_bind_text(statement, column, text.c_str(), -1,
                          SQLITE_TRANSIENT);
    };
    const auto bind_decimal =
        [&bind_text](sqlite3_stmt* statement, int column,
                     const std::optional<tradebox::core::Decimal>& value) {
            if (value)
                bind_text(statement, column, value->ToString());
            else
                sqlite3_bind_null(statement, column);
    };
    bool failed = false;
    std::string failure_error;
    for (const auto& activity : activities) {
        if (activity.account_id.empty() ||
            activity.provider_id.empty()) {
            failed = true;
            failure_error =
                "Account activity account_id and provider_id are required";
            break;
        }
        sqlite3_reset(existing);
        sqlite3_clear_bindings(existing);
        bind_text(existing, 1, activity.account_id);
        bind_text(existing, 2, activity.provider_id);
        const int found = sqlite3_step(existing);
        if (found == SQLITE_ROW) {
            const unsigned char* raw = sqlite3_column_text(existing, 0);
            const std::string previous =
                raw ? reinterpret_cast<const char*>(raw) : "";
            if (previous == activity.raw_payload)
                ++result.unchanged;
            else
                ++result.revised;
        } else if (found == SQLITE_DONE) {
            ++result.inserted;
        } else {
            failed = true;
            failure_error = sqlite3_errmsg(db_);
            break;
        }

        sqlite3_reset(upsert);
        sqlite3_clear_bindings(upsert);
        int column = 1;
        bind_text(upsert, column++, activity.account_id);
        bind_text(upsert, column++, activity.provider_id);
        bind_text(upsert, column++, activity.activity_type);
        bind_text(upsert, column++, activity.activity_subtype);
        bind_text(upsert, column++, activity.execution_type);
        bind_text(upsert, column++, activity.status);
        bind_text(upsert, column++, activity.symbol);
        bind_text(upsert, column++, activity.cusip);
        bind_text(upsert, column++, activity.side);
        bind_text(upsert, column++, activity.order_id);
        bind_text(upsert, column++, activity.currency);
        bind_text(upsert, column++, activity.occurred_at);
        bind_text(upsert, column++, activity.settlement_date);
        sqlite3_bind_int64(upsert, column++, activity.occurred_at_ms);
        bind_decimal(upsert, column++, activity.qty);
        bind_decimal(upsert, column++, activity.price);
        bind_decimal(upsert, column++, activity.cumulative_qty);
        bind_decimal(upsert, column++, activity.leaves_qty);
        bind_decimal(upsert, column++, activity.net_amount);
        bind_decimal(upsert, column++, activity.per_share_amount);
        sqlite3_bind_int(
            upsert, column++,
            static_cast<int>(activity.fill_reconciliation));
        bind_text(upsert, column++, activity.raw_payload);
        sqlite3_bind_int64(upsert, column++, NowMs());
        if (sqlite3_step(upsert) != SQLITE_DONE) {
            failed = true;
            failure_error = sqlite3_errmsg(db_);
            break;
        }
    }
    sqlite3_finalize(existing);
    sqlite3_finalize(upsert);
    if (failed) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected(failure_error);
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
        const std::string error = sqlite3_errmsg(db_);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected(error);
    }
    return result;
}

tradebox::core::AccountActivityPage Database::LoadAccountActivities(
    const tradebox::core::AccountActivityQuery& query) {
    tradebox::core::AccountActivityPage page;
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT account_id,provider_id,activity_type,activity_subtype,"
        "execution_type,status,symbol,cusip,side,order_id,currency,"
        "occurred_at,settlement_date,occurred_at_ms,qty_text,price_text,"
        "cumulative_qty_text,leaves_qty_text,net_amount_text,"
        "per_share_amount_text,fill_reconciliation,raw_payload,revision "
        "FROM account_activities WHERE (?='' OR account_id=?) "
        "AND (?='' OR activity_type=?) AND (?='' OR symbol=?) "
        "AND (?=0 OR occurred_at_ms>?) AND (?=0 OR occurred_at_ms<?) "
        "AND (?=0 OR occurred_at_ms<? OR "
        "(occurred_at_ms=? AND provider_id<?)) "
        "ORDER BY occurred_at_ms DESC,provider_id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) !=
        SQLITE_OK)
        return page;
    int binding = 1;
    const auto bind_twice = [&statement, &binding](
                                const std::string& value) {
        sqlite3_bind_text(statement, binding++, value.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, binding++, value.c_str(), -1,
                          SQLITE_TRANSIENT);
    };
    bind_twice(query.account_id);
    bind_twice(query.activity_type);
    bind_twice(query.symbol);
    sqlite3_bind_int64(statement, binding++, query.after_ms);
    sqlite3_bind_int64(statement, binding++, query.after_ms);
    sqlite3_bind_int64(statement, binding++, query.before_ms);
    sqlite3_bind_int64(statement, binding++, query.before_ms);
    sqlite3_bind_int64(statement, binding++, query.cursor_time_ms);
    sqlite3_bind_int64(statement, binding++, query.cursor_time_ms);
    sqlite3_bind_int64(statement, binding++, query.cursor_time_ms);
    sqlite3_bind_text(
        statement, binding++, query.cursor_provider_id.c_str(), -1,
        SQLITE_TRANSIENT);
    const std::size_t maximum =
        std::clamp<std::size_t>(query.maximum, 1, 1000);
    sqlite3_bind_int(statement, binding, static_cast<int>(maximum));
    const auto text = [statement](int column) {
        const unsigned char* value =
            sqlite3_column_text(statement, column);
        return value
                   ? std::string(reinterpret_cast<const char*>(value))
                   : std::string{};
    };
    const auto decimal = [&text, statement](int column)
        -> std::optional<tradebox::core::Decimal> {
        if (sqlite3_column_type(statement, column) == SQLITE_NULL)
            return std::nullopt;
        auto parsed = tradebox::core::Decimal::Parse(text(column));
        return parsed ? std::optional(*parsed) : std::nullopt;
    };
    while (sqlite3_step(statement) == SQLITE_ROW) {
        page.activities.push_back({
            .account_id = text(0),
            .provider_id = text(1),
            .activity_type = text(2),
            .activity_subtype = text(3),
            .execution_type = text(4),
            .status = text(5),
            .symbol = text(6),
            .cusip = text(7),
            .side = text(8),
            .order_id = text(9),
            .currency = text(10),
            .occurred_at = text(11),
            .settlement_date = text(12),
            .occurred_at_ms = sqlite3_column_int64(statement, 13),
            .qty = decimal(14),
            .price = decimal(15),
            .cumulative_qty = decimal(16),
            .leaves_qty = decimal(17),
            .net_amount = decimal(18),
            .per_share_amount = decimal(19),
            .fill_reconciliation =
                static_cast<tradebox::core::ActivityFillReconciliation>(
                    sqlite3_column_int(statement, 20)),
            .raw_payload = text(21),
            .revision = static_cast<std::uint32_t>(
                sqlite3_column_int(statement, 22)),
        });
    }
    sqlite3_finalize(statement);
    if (page.activities.size() == maximum) {
        page.next_cursor_time_ms =
            page.activities.back().occurred_at_ms;
        page.next_cursor_provider_id =
            page.activities.back().provider_id;
    }
    return page;
}

std::vector<tradebox::core::TradableAsset> Database::LoadAssetCatalog() {
    std::vector<tradebox::core::TradableAsset> result;
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(market_db_, "SELECT value FROM market_metadata WHERE key='asset_catalog'",
                       -1, &statement, nullptr);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        try {
            const auto json = nlohmann::json::parse(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
            for (const auto& item : json) {
                result.push_back({
                    item.value("symbol", ""), item.value("name", ""),
                    item.value("exchange", ""), item.value("active", false),
                    item.value("tradable", false), item.value("shortable", false),
                    item.value("fractionable", false), item.value("previous_volume", 0LL),
                    item.value("previous_dollar_volume", 0LL),
                    item.value("received_at_ms", 0LL),
                    item.value("instrument_id", ""),
                    item.value("isin", ""),
                    item.value("cusip", ""),
                    item.value("sedol", ""),
                    item.value("provider_asset_id",
                               item.value("asset_id", ""))});
                auto& asset = result.back();
                if (asset.instrument_id.empty() &&
                    !asset.provider_asset_id.empty())
                    asset.instrument_id =
                        "alpaca:" + asset.provider_asset_id;
            }
        } catch (...) { result.clear(); }
    }
    sqlite3_finalize(statement);
    return result;
}

std::vector<tradebox::core::TradableAsset>
Database::LoadKnownProviderBarAssets() {
    std::vector<tradebox::core::TradableAsset> result;
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT instrument_id,MAX(symbol) FROM provider_bars "
        "WHERE instrument_id<>'' AND symbol<>'' "
        "GROUP BY instrument_id ORDER BY MAX(symbol)";
    if (sqlite3_prepare_v2(market_db_, sql, -1, &statement, nullptr) !=
        SQLITE_OK)
        return result;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto text = [statement](int column) {
            const auto* value = sqlite3_column_text(statement, column);
            return value
                       ? std::string(
                             reinterpret_cast<const char*>(value))
                       : std::string{};
        };
        result.push_back({
            .symbol = text(1),
            .name = "Stored market data",
            .active = true,
            .tradable = true,
            .instrument_id = text(0),
        });
    }
    sqlite3_finalize(statement);
    return result;
}

void Database::SaveAssetCatalog(
    const std::vector<tradebox::core::TradableAsset>& assets) {
    nlohmann::json json = nlohmann::json::array();
    for (const auto& asset : assets) json.push_back({
        {"symbol", asset.symbol}, {"name", asset.name}, {"exchange", asset.exchange},
        {"active", asset.active}, {"tradable", asset.tradable},
        {"shortable", asset.shortable}, {"fractionable", asset.fractionable},
        {"previous_volume", asset.previous_volume},
        {"previous_dollar_volume", asset.previous_dollar_volume},
        {"received_at_ms", asset.received_at_ms},
        {"instrument_id", asset.instrument_id},
        {"isin", asset.isin},
        {"cusip", asset.cusip},
        {"sedol", asset.sedol},
        {"provider_asset_id", asset.provider_asset_id}});
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(market_db_, "INSERT INTO market_metadata(key,value) VALUES('asset_catalog',?) "
                           "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                       -1, &statement, nullptr);
    const std::string text = json.dump();
    sqlite3_bind_text(statement, 1, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

std::vector<Bar> Database::LoadBars(const std::string& symbol) {
    std::vector<Bar> result;
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        market_db_,
        "SELECT timestamp_ms,open,high,low,close,volume FROM daily_bars "
        "WHERE symbol=? AND feed='iex' ORDER BY timestamp_ms",
        -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        result.push_back({
            sqlite3_column_int64(statement, 0),
            sqlite3_column_double(statement, 1),
            sqlite3_column_double(statement, 2),
            sqlite3_column_double(statement, 3),
            sqlite3_column_double(statement, 4),
            sqlite3_column_double(statement, 5),
        });
    }
    sqlite3_finalize(statement);
    return result;
}

void Database::StoreBars(const std::string& symbol, const std::vector<Bar>& bars) {
    if (bars.empty()) return;
    std::scoped_lock lock(db_mutex_);
    ExecuteMarket("BEGIN IMMEDIATE;");
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        market_db_,
        "INSERT INTO daily_bars(symbol,timestamp_ms,open,high,low,close,volume,feed,timeframe)"
        " VALUES(?,?,?,?,?,?,?,'iex','1Day') ON CONFLICT(symbol,timestamp_ms,feed,timeframe) DO "
        "UPDATE SET open=excluded.open,high=excluded.high,low=excluded.low,"
        "close=excluded.close,volume=excluded.volume",
        -1, &statement, nullptr);
    for (const Bar& bar : bars) {
        sqlite3_bind_text(statement, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 2, bar.timestamp_ms);
        sqlite3_bind_double(statement, 3, bar.open);
        sqlite3_bind_double(statement, 4, bar.high);
        sqlite3_bind_double(statement, 5, bar.low);
        sqlite3_bind_double(statement, 6, bar.close);
        sqlite3_bind_double(statement, 7, bar.volume);
        sqlite3_step(statement);
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);
    ExecuteMarket("COMMIT;");
}

std::expected<void, std::string> Database::StoreProviderBars(
    const tradebox::core::BarUpsertBatch& batch) {
    if (batch.key.instrument_id.empty())
        return std::unexpected(
            "Provider-bar persistence requires instrument identity");
    std::scoped_lock lock(db_mutex_);
    return StoreProviderBarBatchesLocked({batch});
}

bool Database::QueueProviderBars(
    tradebox::core::BarUpsertBatch batch) {
    if (batch.key.instrument_id.empty() || batch.bars.empty())
        return false;
    const std::size_t bar_count = batch.bars.size();
    {
        std::scoped_lock lock(queue_mutex_);
        constexpr std::size_t kMaximumQueuedBars = 50'000;
        if (stopping_ ||
            bar_count > kMaximumQueuedBars - std::min(
                pending_bar_count_, kMaximumQueuedBars)) {
            dropped_bars_ += bar_count;
            return false;
        }
        pending_bar_count_ += bar_count;
        pending_bar_batches_.push_back(std::move(batch));
        accepted_bars_ += bar_count;
        bar_high_water_ = std::max<std::uint64_t>(
            bar_high_water_, pending_bar_count_);
    }
    queue_cv_.notify_one();
    return true;
}

std::expected<void, std::string>
Database::StoreProviderBarBatchesLocked(
    const std::vector<tradebox::core::BarUpsertBatch>& batches) {
    if (batches.empty()) return {};
    std::string error;
    if (!ExecuteMarket("BEGIN IMMEDIATE;", &error))
        return std::unexpected(error);
    const auto rollback = [this](std::string message)
        -> std::expected<void, std::string> {
        ExecuteMarket("ROLLBACK;");
        return std::unexpected(std::move(message));
    };
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
        market_db_,
        "INSERT INTO provider_bars("
        "instrument_id,symbol,feed,timeframe,adjustment,start_ns,"
        "open_text,high_text,low_text,close_text,volume_text,vwap_text,"
        "trade_count,source,state,revision)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(instrument_id,feed,timeframe,adjustment,start_ns)"
        " DO UPDATE SET symbol=excluded.symbol,"
        "open_text=excluded.open_text,high_text=excluded.high_text,"
        "low_text=excluded.low_text,close_text=excluded.close_text,"
        "volume_text=excluded.volume_text,vwap_text=excluded.vwap_text,"
        "trade_count=excluded.trade_count,source=excluded.source,"
        "state=CASE WHEN excluded.state=0 THEN 0 "
        "WHEN provider_bars.state IN (1,2) THEN 2 "
        "ELSE excluded.state END,revision=provider_bars.revision+1"
        " WHERE NOT (provider_bars.source=1"
        " AND provider_bars.state=2"
        " AND excluded.source=1 AND excluded.state=1)"
        " AND (provider_bars.symbol<>excluded.symbol"
        " OR provider_bars.open_text<>excluded.open_text"
        " OR provider_bars.high_text<>excluded.high_text"
        " OR provider_bars.low_text<>excluded.low_text"
        " OR provider_bars.close_text<>excluded.close_text"
        " OR provider_bars.volume_text<>excluded.volume_text"
        " OR provider_bars.vwap_text IS NOT excluded.vwap_text"
        " OR provider_bars.trade_count<>excluded.trade_count"
        " OR provider_bars.source<>excluded.source"
        " OR provider_bars.state<>excluded.state)",
        -1, &statement, nullptr) != SQLITE_OK)
        return rollback(sqlite3_errmsg(market_db_));
    for (const auto& batch : batches) {
        for (const auto& bar : batch.bars) {
            const std::string open = bar.open.ToString();
            const std::string high = bar.high.ToString();
            const std::string low = bar.low.ToString();
            const std::string close = bar.close.ToString();
            const std::string volume = bar.volume.ToString();
            const std::string vwap =
                bar.within_bar_vwap
                    ? bar.within_bar_vwap->ToString()
                    : std::string{};
            sqlite3_bind_text(statement, 1,
                              batch.key.instrument_id.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, batch.symbol.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_int(statement, 3,
                             static_cast<int>(batch.key.feed));
            sqlite3_bind_text(statement, 4,
                              batch.key.timeframe.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_int(
                statement, 5,
                static_cast<int>(batch.key.adjustment));
            sqlite3_bind_int64(statement, 6, bar.start_ns);
            sqlite3_bind_text(statement, 7, open.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 8, high.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 9, low.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 10, close.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 11, volume.c_str(), -1,
                              SQLITE_TRANSIENT);
            if (bar.within_bar_vwap)
                sqlite3_bind_text(statement, 12, vwap.c_str(), -1,
                                  SQLITE_TRANSIENT);
            else
                sqlite3_bind_null(statement, 12);
            sqlite3_bind_int64(
                statement, 13,
                static_cast<sqlite3_int64>(bar.trade_count));
            sqlite3_bind_int(
                statement, 14, static_cast<int>(bar.source));
            sqlite3_bind_int(
                statement, 15, static_cast<int>(bar.state));
            sqlite3_bind_int64(
                statement, 16,
                static_cast<sqlite3_int64>(
                    std::max<std::uint64_t>(bar.revision, 1)));
            if (sqlite3_step(statement) != SQLITE_DONE) {
                error = sqlite3_errmsg(market_db_);
                sqlite3_finalize(statement);
                return rollback(error);
            }
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        }
    }
    sqlite3_finalize(statement);

    if (sqlite3_prepare_v2(
        market_db_,
        "INSERT OR IGNORE INTO provider_bar_coverage("
        "instrument_id,feed,timeframe,adjustment,start_ns,end_ns)"
        " VALUES(?,?,?,?,?,?)",
        -1, &statement, nullptr) != SQLITE_OK)
        return rollback(sqlite3_errmsg(market_db_));
    for (const auto& batch : batches) {
        if (!batch.covered_range ||
            batch.covered_range->start_ns >=
                batch.covered_range->end_ns)
            continue;
        sqlite3_bind_text(statement, 1,
                         batch.key.instrument_id.c_str(), -1,
                         SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2,
                        static_cast<int>(batch.key.feed));
        sqlite3_bind_text(statement, 3,
                         batch.key.timeframe.c_str(), -1,
                         SQLITE_TRANSIENT);
        sqlite3_bind_int(
            statement, 4,
            static_cast<int>(batch.key.adjustment));
        sqlite3_bind_int64(
            statement, 5, batch.covered_range->start_ns);
        sqlite3_bind_int64(
            statement, 6, batch.covered_range->end_ns);
        if (sqlite3_step(statement) != SQLITE_DONE) {
            error = sqlite3_errmsg(market_db_);
            sqlite3_finalize(statement);
            return rollback(error);
        }
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    }
    sqlite3_finalize(statement);
    if (!ExecuteMarket("COMMIT;", &error))
        return rollback(error);
    return {};
}

StoredBarSeries Database::LoadProviderBars(
    const tradebox::core::BarSeriesKey& key,
    tradebox::core::BarRange range) {
    StoredBarSeries result{.key = key};
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        market_db_,
        "SELECT symbol,start_ns,open_text,high_text,low_text,"
        "close_text,volume_text,vwap_text,trade_count,source,state,revision"
        " FROM provider_bars WHERE instrument_id=? AND feed=?"
        " AND timeframe=? AND adjustment=? AND start_ns>=?"
        " AND start_ns<? ORDER BY start_ns",
        -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, key.instrument_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, static_cast<int>(key.feed));
    sqlite3_bind_text(statement, 3, key.timeframe.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4,
                     static_cast<int>(key.adjustment));
    sqlite3_bind_int64(statement, 5, range.start_ns);
    sqlite3_bind_int64(statement, 6, range.end_ns);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto text = [statement](int column) {
            const auto* value = sqlite3_column_text(
                statement, column);
            return value
                       ? std::string(
                             reinterpret_cast<const char*>(value))
                       : std::string{};
        };
        if (result.symbol.empty()) result.symbol = text(0);
        tradebox::core::MarketBar bar{
            .start_ns = sqlite3_column_int64(statement, 1),
            .open = *tradebox::core::Decimal::Parse(text(2)),
            .high = *tradebox::core::Decimal::Parse(text(3)),
            .low = *tradebox::core::Decimal::Parse(text(4)),
            .close = *tradebox::core::Decimal::Parse(text(5)),
            .volume = *tradebox::core::Decimal::Parse(text(6)),
            .trade_count = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement, 8)),
            .source = static_cast<tradebox::core::BarSource>(
                sqlite3_column_int(statement, 9)),
            .state = static_cast<tradebox::core::BarState>(
                sqlite3_column_int(statement, 10)),
            .revision = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement, 11)),
        };
        if (sqlite3_column_type(statement, 7) != SQLITE_NULL)
            bar.within_bar_vwap =
                *tradebox::core::Decimal::Parse(text(7));
        result.bars.push_back(std::move(bar));
    }
    sqlite3_finalize(statement);

    sqlite3_prepare_v2(
        market_db_,
        "SELECT start_ns,end_ns FROM provider_bar_coverage"
        " WHERE instrument_id=? AND feed=? AND timeframe=?"
        " AND adjustment=? AND end_ns>? AND start_ns<?"
        " ORDER BY start_ns,end_ns",
        -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, key.instrument_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, static_cast<int>(key.feed));
    sqlite3_bind_text(statement, 3, key.timeframe.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4,
                     static_cast<int>(key.adjustment));
    sqlite3_bind_int64(statement, 5, range.start_ns);
    sqlite3_bind_int64(statement, 6, range.end_ns);
    while (sqlite3_step(statement) == SQLITE_ROW)
        result.coverage.push_back({
            sqlite3_column_int64(statement, 0),
            sqlite3_column_int64(statement, 1),
        });
    sqlite3_finalize(statement);
    return result;
}

void Database::QueueTimelineEvent(std::string source,
                                  std::string source_event_id,
                                  std::string kind, std::string symbol,
                                  std::int64_t event_time_ms,
                                  std::string payload) {
    {
        std::scoped_lock lock(queue_mutex_);
        constexpr std::size_t kMaximumQueuedEvents = 250'000;
        if (pending_.size() >= kMaximumQueuedEvents) {
            ++dropped_timeline_events_;
            return;
        }
        pending_.push_back(
            {.market_tick = false,
             .source = std::move(source),
             .source_event_id = std::move(source_event_id),
             .kind = std::move(kind),
             .symbol = std::move(symbol),
             .event_time = event_time_ms,
             .available_at_ms = NowMs(),
             .payload = std::move(payload)});
        ++accepted_events_;
        queue_high_water_ =
            std::max<std::uint64_t>(queue_high_water_,
                                    pending_.size());
    }
    queue_cv_.notify_one();
}

bool Database::QueueMarketTickEvent(
    std::string feed, std::string source_event_id, std::string kind,
    std::string instrument_id, std::string symbol,
    std::int64_t event_time_ns,
    std::int64_t received_at_ms, std::string payload) {
    if (instrument_id.empty() || symbol.empty()) return false;
    {
        std::scoped_lock lock(queue_mutex_);
        constexpr std::size_t kMaximumQueuedEvents = 250'000;
        if (pending_.size() >= kMaximumQueuedEvents) {
            ++dropped_market_events_;
            return false;
        }
        pending_.push_back({
            .market_tick = true,
            .source = std::move(feed),
            .source_event_id = std::move(source_event_id),
            .kind = std::move(kind),
            .instrument_id = std::move(instrument_id),
            .symbol = std::move(symbol),
            .event_time = event_time_ns,
            .available_at_ms = received_at_ms,
            .payload = std::move(payload),
        });
        ++accepted_events_;
        queue_high_water_ =
            std::max<std::uint64_t>(queue_high_water_,
                                    pending_.size());
    }
    queue_cv_.notify_one();
    return true;
}

bool Database::QueueMarketDataEvent(
    std::string feed, std::string source_event_id,
    tradebox::core::MarketDataEventPtr event) {
    if (!event) return false;
    const MarketEventMetadata metadata = Metadata(*event);
    if (metadata.kind.empty() || metadata.instrument_id.empty() ||
        metadata.symbol.empty())
        return false;
    {
        std::scoped_lock lock(queue_mutex_);
        constexpr std::size_t kMaximumQueuedEvents = 250'000;
        if (pending_.size() >= kMaximumQueuedEvents) {
            ++dropped_market_events_;
            return false;
        }
        pending_.push_back({
            .market_tick = true,
            .source = std::move(feed),
            .source_event_id = std::move(source_event_id),
            .market_data_event = std::move(event),
        });
        ++accepted_events_;
        queue_high_water_ =
            std::max<std::uint64_t>(queue_high_water_,
                                    pending_.size());
    }
    queue_cv_.notify_one();
    return true;
}

bool Database::QueueMarketDataEvents(
    std::string feed,
    std::vector<QueuedMarketDataEvent> events) {
    if (events.empty()) return true;
    for (const QueuedMarketDataEvent& queued : events) {
        if (!queued.event) return false;
        const MarketEventMetadata metadata =
            Metadata(*queued.event);
        if (metadata.kind.empty() ||
            metadata.instrument_id.empty() ||
            metadata.symbol.empty())
            return false;
    }
    {
        std::scoped_lock lock(queue_mutex_);
        constexpr std::size_t kMaximumQueuedEvents =
            250'000;
        if (events.size() >
            kMaximumQueuedEvents - pending_.size()) {
            dropped_market_events_ += events.size();
            return false;
        }
        for (QueuedMarketDataEvent& queued : events) {
            pending_.push_back({
                .market_tick = true,
                .source = feed,
                .source_event_id =
                    std::move(queued.source_event_id),
                .market_data_event =
                    std::move(queued.event),
            });
        }
        accepted_events_ += events.size();
        queue_high_water_ =
            std::max<std::uint64_t>(
                queue_high_water_, pending_.size());
    }
    queue_cv_.notify_one();
    return true;
}

std::expected<void, std::string> Database::FlushQueuedWrites() {
    std::unique_lock lock(queue_mutex_);
    if (!writer_.joinable())
        return std::unexpected("Market-data persistence writer is not running");
    if (stopping_)
        return std::unexpected("Market-data persistence writer is stopping");
    const std::uint64_t requested = ++flush_requested_;
    queue_cv_.notify_one();
    flush_cv_.wait(lock, [&] {
        return flush_completed_ >= requested || stopping_;
    });
    if (stopping_)
        return std::unexpected("Market-data persistence writer stopped");
    if (!last_write_error_.empty())
        return std::unexpected(last_write_error_);
    return {};
}

std::expected<void, std::string> Database::StoreMarketTickEvents(
    const std::vector<StoredMarketTick>& events) {
    if (events.empty()) return {};
    std::scoped_lock lock(db_mutex_);
    std::string error;
    if (!ExecuteMarket("BEGIN IMMEDIATE;", &error))
        return std::unexpected(error);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
        market_db_,
        "INSERT OR IGNORE INTO market_tick_events("
        "feed,source_event_id,kind,instrument_id,symbol,event_time_ns,"
        "received_at_ms,raw_payload) VALUES(?,?,?,?,?,?,?,?)",
        -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(market_db_);
        ExecuteMarket("ROLLBACK;");
        return std::unexpected(error);
    }
    for (const StoredMarketTick& event : events) {
        if (event.instrument_id.empty() || event.symbol.empty()) {
            sqlite3_finalize(statement);
            ExecuteMarket("ROLLBACK;");
            return std::unexpected(
                "Market tick persistence requires instrument identity "
                "and symbol");
        }
        sqlite3_bind_text(statement, 1, event.feed.c_str(), -1,
                          SQLITE_TRANSIENT);
        if (event.source_event_id.empty())
            sqlite3_bind_null(statement, 2);
        else
            sqlite3_bind_text(statement, 2,
                              event.source_event_id.c_str(), -1,
                              SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, event.kind.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 4, event.instrument_id.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 5, event.symbol.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 6, event.event_time_ns);
        sqlite3_bind_int64(statement, 7, event.received_at_ms);
        sqlite3_bind_text(statement, 8, event.raw_payload.c_str(), -1,
                          SQLITE_TRANSIENT);
        if (sqlite3_step(statement) != SQLITE_DONE) {
            error = sqlite3_errmsg(market_db_);
            sqlite3_finalize(statement);
            ExecuteMarket("ROLLBACK;");
            return std::unexpected(error);
        }
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    }
    sqlite3_finalize(statement);
    if (!ExecuteMarket("COMMIT;", &error)) {
        ExecuteMarket("ROLLBACK;");
        return std::unexpected(error);
    }
    return {};
}

tradebox::core::TickSeries Database::LoadMarketTicks(
    const tradebox::core::TickQuery& query) {
    tradebox::core::TickSeries result{.query = query};
    if (query.instrument_id.empty()) {
        result.error =
            "Tick cache lookup requires stable instrument identity";
        return result;
    }
    std::map<std::string, tradebox::core::MarketTrade> trades;
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
        market_db_,
        "SELECT kind,event_time_ns,received_at_ms,raw_payload,id "
        "FROM market_tick_events WHERE instrument_id=? AND feed=? "
        "AND ((event_time_ns>=? AND event_time_ns<?) "
        "OR kind IN ('x','c')) "
        "ORDER BY event_time_ns,id",
        -1, &statement, nullptr) != SQLITE_OK) {
        result.error = sqlite3_errmsg(market_db_);
        return result;
    }
    const std::string feed = FeedName(query.feed);
    sqlite3_bind_text(statement, 1, query.instrument_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, feed.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, query.start_ns);
    sqlite3_bind_int64(statement, 4, query.end_ns);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const std::string kind = reinterpret_cast<const char*>(
            sqlite3_column_text(statement, 0));
        const std::int64_t event_time_ns =
            sqlite3_column_int64(statement, 1);
        const std::int64_t received_at_ms =
            sqlite3_column_int64(statement, 2);
        const auto* payload_text = sqlite3_column_text(statement, 3);
        if (!payload_text) continue;
        try {
            const nlohmann::json payload = nlohmann::json::parse(
                reinterpret_cast<const char*>(payload_text));
            if (kind == "q" && query.include_quotes) {
                result.quotes.push_back({
                    .instrument_id = query.instrument_id,
                    .symbol = query.symbol,
                    .bid_price = JsonDecimal(payload, "bp"),
                    .bid_size = JsonDecimal(payload, "bs"),
                    .bid_exchange = JsonString(payload, "bx"),
                    .ask_price = JsonDecimal(payload, "ap"),
                    .ask_size = JsonDecimal(payload, "as"),
                    .ask_exchange = JsonString(payload, "ax"),
                    .conditions = JsonStrings(payload, "c"),
                    .tape = JsonString(payload, "z"),
                    .broker_timestamp = JsonString(payload, "t"),
                    .event_time_ns = event_time_ns,
                    .received_at_ms = received_at_ms,
                });
            } else if (kind == "t" && query.include_trades) {
                tradebox::core::MarketTrade trade{
                    .instrument_id = query.instrument_id,
                    .symbol = query.symbol,
                    .trade_id = JsonIdentifier(payload, "i"),
                    .price = JsonDecimal(payload, "p"),
                    .size = JsonDecimal(payload, "s"),
                    .exchange = JsonString(payload, "x"),
                    .conditions = JsonStrings(payload, "c"),
                    .tape = JsonString(payload, "z"),
                    .broker_timestamp = JsonString(payload, "t"),
                    .event_time_ns = event_time_ns,
                    .received_at_ms = received_at_ms,
                };
                std::string key = trade.trade_id;
                if (key.empty())
                    key = "row:" + std::to_string(
                                       sqlite3_column_int64(statement, 4));
                else
                    key = TickTradeKey(key, event_time_ns);
                trades.insert_or_assign(std::move(key), std::move(trade));
            } else if (kind == "x" && query.include_trades) {
                const std::string canceled_id =
                    JsonIdentifier(payload, "i");
                const std::size_t erased = std::erase_if(
                    trades, [&](const auto& entry) {
                        return entry.second.trade_id == canceled_id &&
                               entry.second.event_time_ns / kDayNs ==
                                   event_time_ns / kDayNs;
                    });
                static_cast<void>(erased);
            } else if (kind == "c" && query.include_trades) {
                const std::string original_id =
                    JsonIdentifier(payload, "oi");
                auto original = std::ranges::find_if(
                    trades, [&](const auto& entry) {
                        return entry.second.trade_id == original_id &&
                               entry.second.event_time_ns / kDayNs ==
                                   event_time_ns / kDayNs;
                    });
                if (original == trades.end()) continue;
                const std::int64_t original_event_time_ns =
                    original->second.event_time_ns;
                trades.erase(original);
                tradebox::core::MarketTrade corrected{
                    .instrument_id = query.instrument_id,
                    .symbol = query.symbol,
                    .trade_id = JsonIdentifier(payload, "ci"),
                    .price = JsonDecimal(payload, "cp"),
                    .size = JsonDecimal(payload, "cs"),
                    .exchange = JsonString(payload, "x"),
                    .conditions = JsonStrings(payload, "cc"),
                    .tape = JsonString(payload, "z"),
                    .broker_timestamp = JsonString(payload, "t"),
                    .event_time_ns = original_event_time_ns,
                    .received_at_ms = received_at_ms,
                    .corrected = true,
                };
                trades.insert_or_assign(
                    TickTradeKey(corrected.trade_id,
                                 original_event_time_ns),
                    std::move(corrected));
            }
        } catch (const std::exception& error) {
            if (result.error.empty())
                result.error =
                    "Stored raw market tick is invalid: " +
                    std::string(error.what());
        }
    }
    sqlite3_finalize(statement);

    if (sqlite3_prepare_v2(
        market_db_,
        "SELECT kind,instrument_id,event_time_ns,received_at_ms,"
        "broker_timestamp,trade_id,original_trade_id,price_text,"
        "size_text,exchange,conditions_text,tape,bid_price_text,"
        "bid_size_text,bid_exchange,ask_price_text,ask_size_text,"
        "ask_exchange,corrected,id "
        "FROM typed_market_ticks WHERE instrument_id=? AND feed=? "
        "AND ((event_time_ns>=? AND event_time_ns<?) "
        "OR kind IN ('x','c')) ORDER BY event_time_ns,id",
        -1, &statement, nullptr) != SQLITE_OK) {
        result.error = sqlite3_errmsg(market_db_);
        return result;
    }
    sqlite3_bind_text(statement, 1, query.instrument_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, feed.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, query.start_ns);
    sqlite3_bind_int64(statement, 4, query.end_ns);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto text = [statement](int column) {
            const auto* value =
                sqlite3_column_text(statement, column);
            return value
                       ? std::string(
                             reinterpret_cast<const char*>(value))
                       : std::string{};
        };
        const auto decimal = [&](int column) {
            const auto parsed =
                tradebox::core::Decimal::Parse(text(column));
            if (!parsed && result.error.empty())
                result.error =
                    "Stored typed market tick contains an invalid "
                    "decimal";
            return parsed ? *parsed
                          : tradebox::core::Decimal::Zero();
        };
        const std::string kind = text(0);
        const std::string instrument_id = text(1);
        const std::int64_t event_time_ns =
            sqlite3_column_int64(statement, 2);
        const std::int64_t received_at_ms =
            sqlite3_column_int64(statement, 3);
        if (kind == "q" && query.include_quotes) {
            result.quotes.push_back({
                .instrument_id = instrument_id,
                .symbol = query.symbol,
                .bid_price = decimal(12),
                .bid_size = decimal(13),
                .bid_exchange = text(14),
                .ask_price = decimal(15),
                .ask_size = decimal(16),
                .ask_exchange = text(17),
                .conditions = DecodeConditions(text(10)),
                .tape = text(11),
                .broker_timestamp = text(4),
                .event_time_ns = event_time_ns,
                .received_at_ms = received_at_ms,
            });
        } else if (kind == "t" && query.include_trades) {
            tradebox::core::MarketTrade trade{
                .instrument_id = instrument_id,
                .symbol = query.symbol,
                .trade_id = text(5),
                .price = decimal(7),
                .size = decimal(8),
                .exchange = text(9),
                .conditions = DecodeConditions(text(10)),
                .tape = text(11),
                .broker_timestamp = text(4),
                .event_time_ns = event_time_ns,
                .received_at_ms = received_at_ms,
                .corrected =
                    sqlite3_column_int(statement, 18) != 0,
            };
            std::string key =
                trade.trade_id.empty()
                    ? "typed-row:" +
                          std::to_string(
                              sqlite3_column_int64(statement, 19))
                    : TickTradeKey(trade.trade_id,
                                   event_time_ns);
            trades.insert_or_assign(std::move(key),
                                    std::move(trade));
        } else if (kind == "x" && query.include_trades) {
            const std::string canceled_id = text(5);
            const std::size_t erased = std::erase_if(
                trades, [&](const auto& entry) {
                    return entry.second.trade_id == canceled_id &&
                           entry.second.event_time_ns / kDayNs ==
                               event_time_ns / kDayNs;
                });
            static_cast<void>(erased);
        } else if (kind == "c" && query.include_trades) {
            const std::string original_id = text(6);
            auto original = std::ranges::find_if(
                trades, [&](const auto& entry) {
                    return entry.second.trade_id == original_id &&
                           entry.second.event_time_ns / kDayNs ==
                               event_time_ns / kDayNs;
                });
            if (original == trades.end()) continue;
            const std::int64_t original_event_time_ns =
                original->second.event_time_ns;
            trades.erase(original);
            tradebox::core::MarketTrade corrected{
                .instrument_id = instrument_id,
                .symbol = query.symbol,
                .trade_id = text(5),
                .price = decimal(7),
                .size = decimal(8),
                .exchange = text(9),
                .conditions = DecodeConditions(text(10)),
                .tape = text(11),
                .broker_timestamp = text(4),
                .event_time_ns = original_event_time_ns,
                .received_at_ms = received_at_ms,
                .corrected = true,
            };
            trades.insert_or_assign(
                TickTradeKey(corrected.trade_id,
                             original_event_time_ns),
                std::move(corrected));
        }
    }
    sqlite3_finalize(statement);
    result.trades.reserve(trades.size());
    for (auto& [id, trade] : trades) {
        static_cast<void>(id);
        result.trades.push_back(std::move(trade));
    }
    std::ranges::sort(result.trades, {}, &tradebox::core::MarketTrade::event_time_ns);
    std::ranges::sort(result.quotes, {}, &tradebox::core::MarketQuote::event_time_ns);
    return result;
}

std::vector<tradebox::core::TickCoverage>
Database::MissingMarketTickCoverage(
    const tradebox::core::TickQuery& query, std::string_view kind) {
    if (query.instrument_id.empty())
        return {{query.start_ns, query.end_ns}};
    std::vector<tradebox::core::TickCoverage> covered;
    {
        std::scoped_lock lock(db_mutex_);
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(
            market_db_,
            "SELECT start_ns,end_ns FROM market_tick_coverage_v2 "
            "WHERE instrument_id=? AND feed=? AND kind=? AND end_ns>=? "
            "AND start_ns<=? ORDER BY start_ns,end_ns",
            -1, &statement, nullptr) != SQLITE_OK)
            return {{query.start_ns, query.end_ns}};
        const std::string feed = FeedName(query.feed);
        sqlite3_bind_text(statement, 1, query.instrument_id.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, feed.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, kind.data(),
                          static_cast<int>(kind.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 4, query.start_ns);
        sqlite3_bind_int64(statement, 5, query.end_ns);
        while (sqlite3_step(statement) == SQLITE_ROW)
            covered.push_back({
                sqlite3_column_int64(statement, 0),
                sqlite3_column_int64(statement, 1),
            });
        sqlite3_finalize(statement);
    }
    std::vector<tradebox::core::TickCoverage> missing;
    std::int64_t cursor = query.start_ns;
    for (const auto& interval : covered) {
        if (interval.end_ns < cursor) continue;
        if (interval.start_ns > cursor)
            missing.push_back({
                cursor, std::min(query.end_ns, interval.start_ns)});
        cursor = std::max(cursor, interval.end_ns);
        if (cursor >= query.end_ns) break;
    }
    if (cursor < query.end_ns)
        missing.push_back({cursor, query.end_ns});
    return missing;
}

std::expected<void, std::string> Database::MarkMarketTickCoverage(
    const tradebox::core::TickQuery& query, std::string_view kind,
    tradebox::core::TickCoverage coverage) {
    if (query.instrument_id.empty())
        return std::unexpected(
            "Tick coverage requires stable instrument identity");
    if (coverage.start_ns >= coverage.end_ns)
        return std::unexpected("Tick coverage range is invalid");
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
        market_db_,
        "INSERT OR IGNORE INTO market_tick_coverage_v2("
        "instrument_id,symbol,feed,kind,start_ns,end_ns,completed_at_ms)"
        " VALUES(?,?,?,?,?,?,?)",
        -1, &statement, nullptr) != SQLITE_OK)
        return std::unexpected(std::string(sqlite3_errmsg(market_db_)));
    const std::string feed = FeedName(query.feed);
    sqlite3_bind_text(statement, 1, query.instrument_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, query.symbol.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, feed.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, kind.data(),
                      static_cast<int>(kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 5, coverage.start_ns);
    sqlite3_bind_int64(statement, 6, coverage.end_ns);
    sqlite3_bind_int64(statement, 7, NowMs());
    const int rc = sqlite3_step(statement);
    const std::string error =
        rc == SQLITE_DONE ? std::string{} : sqlite3_errmsg(market_db_);
    sqlite3_finalize(statement);
    if (rc != SQLITE_DONE) return std::unexpected(error);
    return {};
}

MarketDataStorageUsage Database::LoadMarketDataStorageUsage() {
    MarketDataStorageUsage usage;
    std::scoped_lock lock(db_mutex_);
    const auto scalar = [this](const char* sql) -> std::uint64_t {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(market_db_, sql, -1, &statement, nullptr) !=
            SQLITE_OK)
            return 0;
        std::uint64_t result = 0;
        if (sqlite3_step(statement) == SQLITE_ROW)
            result = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement, 0));
        sqlite3_finalize(statement);
        return result;
    };
    usage.candlestick_rows =
        scalar("SELECT (SELECT count(*) FROM daily_bars) + "
               "(SELECT count(*) FROM provider_bars)");
    usage.tick_rows =
        scalar("SELECT (SELECT count(*) FROM market_tick_events) + "
               "(SELECT count(*) FROM typed_market_ticks)");
    usage.candlestick_bytes = scalar(
        "SELECT coalesce(sum(pgsize),0) FROM dbstat "
        "WHERE name IN ('daily_bars',"
        "'sqlite_autoindex_daily_bars_1',"
        "'provider_bars',"
        "'sqlite_autoindex_provider_bars_1',"
        "'provider_bar_coverage',"
        "'sqlite_autoindex_provider_bar_coverage_1')");
    usage.tick_bytes = scalar(
        "SELECT coalesce(sum(pgsize),0) FROM dbstat "
        "WHERE name IN ('market_tick_events',"
        "'market_tick_events_source',"
        "'market_tick_events_series',"
        "'market_tick_events_corrections',"
        "'typed_market_ticks',"
        "'typed_market_ticks_source',"
        "'typed_market_ticks_series',"
        "'typed_market_ticks_corrections',"
        "'market_tick_coverage',"
        "'sqlite_autoindex_market_tick_coverage_1',"
        "'market_tick_coverage_v2',"
        "'sqlite_autoindex_market_tick_coverage_v2_1')");

    std::error_code error;
    for (const std::filesystem::path path : {
             data_directory_ / L"market_data.db",
             data_directory_ / L"market_data.db-wal",
             data_directory_ / L"market_data.db-shm"}) {
        if (std::filesystem::exists(path, error)) {
            usage.database_bytes +=
                std::filesystem::file_size(path, error);
            if (error) error.clear();
        } else {
            error.clear();
        }
    }
    return usage;
}

DatabaseWriterTelemetry Database::WriterTelemetry() const {
    std::scoped_lock lock(queue_mutex_);
    return {
        .pending_events = pending_.size(),
        .high_water_events = queue_high_water_,
        .accepted_events = accepted_events_,
        .dequeued_events = dequeued_events_,
        .event_write_batches = event_write_batches_,
        .event_write_nanoseconds = event_write_nanoseconds_,
        .dropped_market_events = dropped_market_events_,
        .dropped_timeline_events = dropped_timeline_events_,
        .pending_bar_batches = pending_bar_batches_.size(),
        .pending_bars = pending_bar_count_,
        .high_water_bars = bar_high_water_,
        .accepted_bars = accepted_bars_,
        .dequeued_bars = dequeued_bars_,
        .dropped_bars = dropped_bars_,
        .write_failures = write_failures_,
        .last_write_error = last_write_error_,
    };
}

void Database::WriterLoop() {
    constexpr auto kMaximumWriteDelay = std::chrono::seconds(5);
    constexpr std::size_t kMaximumBatchSize = 50'000;
    constexpr std::size_t kMaximumBarBatchSize = 10'000;
    for (;;) {
        std::vector<PendingEvent> batch;
        std::vector<tradebox::core::BarUpsertBatch> bar_batches;
        std::uint64_t flush_target = 0;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return stopping_ || !pending_.empty() ||
                       !pending_bar_batches_.empty() ||
                       flush_requested_ > flush_completed_;
            });
            if (!stopping_ && pending_.size() < kMaximumBatchSize &&
                pending_bar_count_ < kMaximumBarBatchSize &&
                flush_requested_ == flush_completed_) {
                queue_cv_.wait_for(
                    lock, kMaximumWriteDelay,
                    [this, kMaximumBatchSize,
                     kMaximumBarBatchSize] {
                    return stopping_ ||
                           pending_.size() >= kMaximumBatchSize ||
                           pending_bar_count_ >=
                               kMaximumBarBatchSize ||
                           flush_requested_ >
                               flush_completed_;
                });
            }
            batch.reserve(std::min(pending_.size(), kMaximumBatchSize));
            while (!pending_.empty() &&
                   batch.size() < kMaximumBatchSize) {
                batch.push_back(std::move(pending_.front()));
                pending_.pop_front();
            }
            dequeued_events_ += batch.size();
            std::size_t bar_count = 0;
            while (!pending_bar_batches_.empty()) {
                const std::size_t next_count =
                    pending_bar_batches_.front().bars.size();
                if (!bar_batches.empty() &&
                    bar_count + next_count >
                        kMaximumBarBatchSize)
                    break;
                bar_count += next_count;
                bar_batches.push_back(
                    std::move(pending_bar_batches_.front()));
                pending_bar_batches_.pop_front();
            }
            pending_bar_count_ -= bar_count;
            dequeued_bars_ += bar_count;
            if (pending_.empty() &&
                pending_bar_batches_.empty())
                flush_target = flush_requested_;
            if (batch.empty() && bar_batches.empty() &&
                stopping_)
                break;
        }
        const auto event_write_start =
            std::chrono::steady_clock::now();
        const auto event_write = FlushEvents(batch);
        const auto event_write_elapsed =
            std::chrono::steady_clock::now() -
            event_write_start;
        const auto bar_write = FlushProviderBars(bar_batches);
        if (!batch.empty()) {
            std::scoped_lock lock(queue_mutex_);
            ++event_write_batches_;
            event_write_nanoseconds_ +=
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                        event_write_elapsed)
                        .count());
        }
        if (!event_write || !bar_write) {
            std::scoped_lock lock(queue_mutex_);
            ++write_failures_;
            last_write_error_ =
                !event_write ? event_write.error()
                             : bar_write.error();
        }
        if (flush_target != 0) {
            {
                std::scoped_lock lock(queue_mutex_);
                flush_completed_ =
                    std::max(flush_completed_, flush_target);
            }
            flush_cv_.notify_all();
        }
    }
    std::vector<PendingEvent> remaining;
    std::vector<tradebox::core::BarUpsertBatch>
        remaining_bar_batches;
    {
        std::scoped_lock lock(queue_mutex_);
        while (!pending_.empty()) {
            remaining.push_back(std::move(pending_.front()));
            pending_.pop_front();
        }
        while (!pending_bar_batches_.empty()) {
            dequeued_bars_ +=
                pending_bar_batches_.front().bars.size();
            remaining_bar_batches.push_back(
                std::move(pending_bar_batches_.front()));
            pending_bar_batches_.pop_front();
        }
        pending_bar_count_ = 0;
    }
    const auto event_write_start =
        std::chrono::steady_clock::now();
    const auto event_write = FlushEvents(remaining);
    const auto event_write_elapsed =
        std::chrono::steady_clock::now() -
        event_write_start;
    const auto bar_write = FlushProviderBars(remaining_bar_batches);
    {
        std::scoped_lock lock(queue_mutex_);
        if (!remaining.empty()) {
            ++event_write_batches_;
            event_write_nanoseconds_ +=
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                        event_write_elapsed)
                        .count());
        }
        if (!event_write || !bar_write) {
            ++write_failures_;
            last_write_error_ =
                !event_write ? event_write.error()
                             : bar_write.error();
        }
        flush_completed_ = flush_requested_;
    }
    flush_cv_.notify_all();
}

std::expected<void, std::string> Database::FlushEvents(
    std::vector<PendingEvent>& events) {
    if (events.empty()) return {};
    std::scoped_lock lock(db_mutex_);
    const auto flush_destination =
        [&](bool market_tick) -> std::expected<void, std::string> {
            std::string error;
            const bool began =
                market_tick
                    ? ExecuteMarket("BEGIN IMMEDIATE;", &error)
                    : Execute("BEGIN IMMEDIATE;", &error);
            if (!began) return std::unexpected(error);
            sqlite3_stmt* statement = nullptr;
            const char* sql =
                market_tick
                    ? "INSERT OR IGNORE INTO market_tick_events("
                      "feed,source_event_id,kind,instrument_id,symbol,"
                      "event_time_ns,received_at_ms,raw_payload)"
                      " VALUES(?,?,?,?,?,?,?,?)"
                    : "INSERT OR IGNORE INTO timeline_events("
                      "source,source_event_id,kind,symbol,event_time_ms,"
                      "available_at_ms,recorded_at_ms,payload_json,"
                      "schema_version) VALUES(?,?,?,?,?,?,?,?,1)";
            sqlite3* connection = market_tick ? market_db_ : db_;
            if (sqlite3_prepare_v2(
                    connection, sql, -1, &statement, nullptr) !=
                SQLITE_OK) {
                error = sqlite3_errmsg(connection);
                if (market_tick)
                    ExecuteMarket("ROLLBACK;");
                else
                    Execute("ROLLBACK;");
                return std::unexpected(error);
            }
            for (const PendingEvent& event : events) {
                if (event.market_tick != market_tick) continue;
                if (market_tick && event.market_data_event)
                    continue;
                BindStaticText(statement, 1, event.source);
                BindNullableStaticText(
                    statement, 2, &event.source_event_id);
                BindStaticText(statement, 3, event.kind);
                if (market_tick) {
                    BindStaticText(
                        statement, 4, event.instrument_id);
                    BindStaticText(
                        statement, 5, event.symbol);
                    sqlite3_bind_int64(statement, 6, event.event_time);
                    sqlite3_bind_int64(
                        statement, 7, event.available_at_ms);
                    BindStaticText(statement, 8, event.payload);
                } else {
                    BindStaticText(statement, 4, event.symbol);
                    sqlite3_bind_int64(statement, 5, event.event_time);
                    sqlite3_bind_int64(
                        statement, 6, event.available_at_ms);
                    sqlite3_bind_int64(statement, 7, NowMs());
                    BindStaticText(statement, 8, event.payload);
                }
                if (sqlite3_step(statement) != SQLITE_DONE) {
                    error = sqlite3_errmsg(connection);
                    sqlite3_finalize(statement);
                    if (market_tick)
                        ExecuteMarket("ROLLBACK;");
                    else
                        Execute("ROLLBACK;");
                    return std::unexpected(error);
                }
                sqlite3_reset(statement);
                sqlite3_clear_bindings(statement);
            }
            sqlite3_finalize(statement);
            const bool committed =
                market_tick ? ExecuteMarket("COMMIT;", &error)
                            : Execute("COMMIT;", &error);
            if (!committed) {
                if (market_tick)
                    ExecuteMarket("ROLLBACK;");
                else
                    Execute("ROLLBACK;");
                return std::unexpected(error);
            }
            return {};
        };
    bool has_timeline = false;
    bool has_raw_ticks = false;
    bool has_typed_ticks = false;
    for (const PendingEvent& event : events) {
        has_raw_ticks =
            has_raw_ticks ||
            (event.market_tick && !event.market_data_event);
        has_typed_ticks =
            has_typed_ticks ||
            (event.market_tick &&
             static_cast<bool>(event.market_data_event));
        has_timeline = has_timeline || !event.market_tick;
    }
    if (has_timeline) {
        const auto written = flush_destination(false);
        if (!written) return written;
    }
    if (has_raw_ticks) {
        const auto written = flush_destination(true);
        if (!written) return written;
    }
    if (!has_typed_ticks) return {};

    std::string error;
    if (!ExecuteMarket("BEGIN IMMEDIATE;", &error))
        return std::unexpected(error);
    constexpr std::size_t kRowsPerStatement = 128;
    constexpr int kColumnsPerRow = 22;
    const auto insert_sql = [](std::size_t rows) {
        std::string sql =
            "INSERT OR IGNORE INTO typed_market_ticks("
            "feed,source_event_id,kind,instrument_id,symbol,"
            "event_time_ns,received_at_ms,broker_timestamp,"
            "trade_id,original_trade_id,price_text,size_text,"
            "exchange,conditions_text,tape,bid_price_text,"
            "bid_size_text,bid_exchange,ask_price_text,"
            "ask_size_text,ask_exchange,corrected) VALUES";
        for (std::size_t row = 0; row < rows; ++row) {
            if (row != 0) sql.push_back(',');
            sql += "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
        }
        return sql;
    };
    sqlite3_stmt* full_statement = nullptr;
    sqlite3_stmt* tail_statement = nullptr;
    const auto prepare =
        [&](std::size_t rows,
            sqlite3_stmt** statement) -> bool {
            const std::string sql = insert_sql(rows);
            return sqlite3_prepare_v2(
                       market_db_, sql.c_str(),
                       static_cast<int>(sql.size()),
                       statement, nullptr) == SQLITE_OK;
        };
    struct SerializedTypedTick {
        const PendingEvent* pending = nullptr;
        std::string_view kind;
        const std::string* instrument_id = nullptr;
        const std::string* symbol = nullptr;
        std::int64_t event_time_ns = 0;
        std::int64_t received_at_ms = 0;
        const std::string* broker_timestamp = nullptr;
        const std::string* trade_id = nullptr;
        const std::string* original_trade_id = nullptr;
        std::string price;
        std::string size;
        const std::string* exchange = nullptr;
        std::string conditions;
        const std::string* tape = nullptr;
        std::string bid_price;
        std::string bid_size;
        const std::string* bid_exchange = nullptr;
        std::string ask_price;
        std::string ask_size;
        const std::string* ask_exchange = nullptr;
        bool corrected = false;
    };
    std::vector<const PendingEvent*> typed_events;
    typed_events.reserve(events.size());
    for (const PendingEvent& pending : events) {
        if (pending.market_data_event)
            typed_events.push_back(&pending);
    }
    for (std::size_t offset = 0;
         offset < typed_events.size();
         offset += kRowsPerStatement) {
        const std::size_t row_count =
            std::min(kRowsPerStatement,
                     typed_events.size() - offset);
        sqlite3_stmt** statement_slot = nullptr;
        if (row_count == kRowsPerStatement) {
            statement_slot = &full_statement;
        } else {
            statement_slot = &tail_statement;
        }
        if (!*statement_slot &&
            !prepare(row_count, statement_slot)) {
            error = sqlite3_errmsg(market_db_);
            sqlite3_finalize(full_statement);
            sqlite3_finalize(tail_statement);
            ExecuteMarket("ROLLBACK;");
            return std::unexpected(error);
        }
        sqlite3_stmt* statement = *statement_slot;
        std::vector<SerializedTypedTick> serialized;
        serialized.reserve(row_count);
        for (std::size_t row = 0; row < row_count; ++row) {
            const PendingEvent& pending = *typed_events[offset + row];
            SerializedTypedTick value{
                .pending = &pending,
            };
            const tradebox::core::MarketQuote* quote = nullptr;
            const tradebox::core::MarketTrade* trade = nullptr;
            const tradebox::core::TradeCanceled* canceled = nullptr;
            const tradebox::core::TradeCorrected* corrected = nullptr;
            std::visit(
                [&](const auto& typed) {
                    using T = std::decay_t<decltype(typed)>;
                    if constexpr (std::is_same_v<
                                      T, tradebox::core::QuoteReceived>) {
                        quote = &typed.quote;
                        value.kind = "q";
                    } else if constexpr (std::is_same_v<
                                             T,
                                             tradebox::core::TradeReceived>) {
                        trade = &typed.trade;
                        value.kind = "t";
                    } else if constexpr (std::is_same_v<
                                             T,
                                             tradebox::core::TradeCanceled>) {
                        canceled = &typed;
                        value.kind = "x";
                    } else if constexpr (std::is_same_v<
                                             T,
                                             tradebox::core::TradeCorrected>) {
                        corrected = &typed;
                        trade = &typed.corrected_trade;
                        value.kind = "c";
                    }
                },
                *pending.market_data_event);
            value.instrument_id = quote       ? &quote->instrument_id
                                  : trade     ? &trade->instrument_id
                                  : canceled  ? &canceled->instrument_id
                                  : corrected ? &corrected->instrument_id
                                              : nullptr;
            value.symbol = quote       ? &quote->symbol
                           : trade     ? &trade->symbol
                           : canceled  ? &canceled->symbol
                           : corrected ? &corrected->symbol
                                       : nullptr;
            value.event_time_ns = quote      ? quote->event_time_ns
                                  : trade    ? trade->event_time_ns
                                  : canceled ? canceled->event_time_ns
                                             : 0;
            value.received_at_ms = quote      ? quote->received_at_ms
                                   : trade    ? trade->received_at_ms
                                   : canceled ? canceled->received_at_ms
                                              : 0;
            value.broker_timestamp = quote      ? &quote->broker_timestamp
                                     : trade    ? &trade->broker_timestamp
                                     : canceled ? &canceled->broker_timestamp
                                                : nullptr;
            value.trade_id = trade      ? &trade->trade_id
                             : canceled ? &canceled->trade_id
                                        : nullptr;
            value.original_trade_id =
                corrected ? &corrected->original_trade_id : nullptr;
            value.price = trade ? trade->price.ToString() : std::string{};
            value.size = trade ? trade->size.ToString() : std::string{};
            value.bid_price =
                quote ? quote->bid_price.ToString() : std::string{};
            value.bid_size = quote ? quote->bid_size.ToString() : std::string{};
            value.ask_price =
                quote ? quote->ask_price.ToString() : std::string{};
            value.ask_size = quote ? quote->ask_size.ToString() : std::string{};
            value.conditions = quote   ? EncodeConditions(quote->conditions)
                               : trade ? EncodeConditions(trade->conditions)
                                       : std::string{};
            value.exchange = trade ? &trade->exchange : nullptr;
            value.tape = quote ? &quote->tape : trade ? &trade->tape : nullptr;
            value.bid_exchange = quote ? &quote->bid_exchange : nullptr;
            value.ask_exchange = quote ? &quote->ask_exchange : nullptr;
            value.corrected = corrected || (trade && trade->corrected);
            serialized.push_back(std::move(value));
        }
        int column = 1;
        for (const SerializedTypedTick& value : serialized) {
            const auto bind_text = [statement](int destination,
                                               const std::string* text) {
                BindNullableStaticText(statement, destination, text);
            };
            BindStaticText(statement, column++, value.pending->source);
            bind_text(column++, &value.pending->source_event_id);
            BindStaticText(statement, column++, value.kind);
            bind_text(column++, value.instrument_id);
            bind_text(column++, value.symbol);
            sqlite3_bind_int64(statement, column++, value.event_time_ns);
            sqlite3_bind_int64(statement, column++, value.received_at_ms);
            bind_text(column++, value.broker_timestamp);
            bind_text(column++, value.trade_id);
            bind_text(column++, value.original_trade_id);
            bind_text(column++, &value.price);
            bind_text(column++, &value.size);
            bind_text(column++, value.exchange);
            bind_text(column++, &value.conditions);
            bind_text(column++, value.tape);
            bind_text(column++, &value.bid_price);
            bind_text(column++, &value.bid_size);
            bind_text(column++, value.bid_exchange);
            bind_text(column++, &value.ask_price);
            bind_text(column++, &value.ask_size);
            bind_text(column++, value.ask_exchange);
            sqlite3_bind_int(
                statement, column++,
                value.corrected ? 1 : 0);
        }
        if (column !=
            static_cast<int>(
                row_count * kColumnsPerRow + 1)) {
            sqlite3_finalize(full_statement);
            sqlite3_finalize(tail_statement);
            ExecuteMarket("ROLLBACK;");
            return std::unexpected(
                "Typed market tick bind count mismatch");
        }
        if (sqlite3_step(statement) != SQLITE_DONE) {
            error = sqlite3_errmsg(market_db_);
            sqlite3_finalize(full_statement);
            sqlite3_finalize(tail_statement);
            ExecuteMarket("ROLLBACK;");
            return std::unexpected(error);
        }
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    }
    sqlite3_finalize(full_statement);
    sqlite3_finalize(tail_statement);
    if (!ExecuteMarket("COMMIT;", &error)) {
        ExecuteMarket("ROLLBACK;");
        return std::unexpected(error);
    }
    return {};
}

std::expected<void, std::string> Database::FlushProviderBars(
    const std::vector<tradebox::core::BarUpsertBatch>& batches) {
    if (batches.empty()) return {};
    std::scoped_lock lock(db_mutex_);
    return StoreProviderBarBatchesLocked(batches);
}
