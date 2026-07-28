#include "tradebox/persistence/database.h"

#include <windows.h>
#include <shlobj.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <type_traits>

namespace {

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
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
                   } else {
                       value["kind"] = "replace";
                       value["order_id"] = typed.order_id;
                       value["replacement"] =
                           ReplacementJson(typed.replacement);
                   }
                   return value;
               },
               command)
        .dump();
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
    return OpenAt(data_directory_ / L"tradebox.db", error);
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
    const int rc = sqlite3_open_v2(
        database_path.string().c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        error = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_busy_timeout(db_, 3000);
    const char* schema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
CREATE TABLE IF NOT EXISTS watchlist (
  symbol TEXT PRIMARY KEY,
  sort_order INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS window_placement (
  id INTEGER PRIMARY KEY CHECK(id = 1),
  x INTEGER NOT NULL,
  y INTEGER NOT NULL,
  width INTEGER NOT NULL,
  height INTEGER NOT NULL,
  maximized INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS app_settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS account_aliases (
  account_id TEXT PRIMARY KEY,
  account_number TEXT NOT NULL,
  alias TEXT NOT NULL,
  updated_at_ms INTEGER NOT NULL
);
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
  completed_at_ms INTEGER,
  outcome INTEGER,
  broker_order_id TEXT,
  http_status INTEGER,
  message TEXT,
  raw_response TEXT,
  payload_json TEXT NOT NULL,
  schema_version INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX IF NOT EXISTS order_commands_account_time
  ON order_commands(account_id, created_at_ms);
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
        "SELECT outcome,broker_order_id,http_status,message,raw_response "
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
        "broker_order_id=?,http_status=?,message=?,raw_response=? "
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
    sqlite3_bind_text(statement, 7, result.request_id.c_str(), -1,
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

std::expected<tradebox::core::OrderCommandLookup, std::string>
Database::LookupOrderCommand(const std::string& request_id) {
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT outcome,broker_order_id,http_status,message,raw_response "
        "FROM order_commands WHERE request_id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) !=
        SQLITE_OK)
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    sqlite3_bind_text(statement, 1, request_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    tradebox::core::OrderCommandLookup result;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        result.exists = true;
        if (sqlite3_column_type(statement, 0) != SQLITE_NULL) {
        const auto text = [statement](int column) {
            const unsigned char* value =
                sqlite3_column_text(statement, column);
            return value ? std::string(
                               reinterpret_cast<const char*>(value))
                         : std::string{};
        };
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
        };
        }
    }
    sqlite3_finalize(statement);
    return result;
}

std::vector<std::string> Database::LoadWatchlist() {
    std::vector<std::string> result;
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db_,
                       "SELECT symbol FROM watchlist ORDER BY sort_order", -1,
                       &statement, nullptr);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        result.emplace_back(
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
    }
    sqlite3_finalize(statement);
    return result;
}

void Database::SaveWatchlist(const std::vector<std::string>& symbols) {
    std::scoped_lock lock(db_mutex_);
    Execute("BEGIN IMMEDIATE; DELETE FROM watchlist;");
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        db_, "INSERT INTO watchlist(symbol, sort_order) VALUES(?, ?)", -1,
        &statement, nullptr);
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        sqlite3_bind_text(statement, 1, symbols[i].c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, static_cast<int>(i));
        sqlite3_step(statement);
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);
    Execute("COMMIT;");
}

std::optional<bool> Database::LoadLastConnectedPaper() {
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        db_,
        "SELECT value FROM app_settings WHERE key='last_connected_account'", -1,
        &statement, nullptr);
    std::optional<bool> result;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const char* value = reinterpret_cast<const char*>(
            sqlite3_column_text(statement, 0));
        if (value && std::strcmp(value, "paper") == 0)
            result = true;
        else if (value && std::strcmp(value, "live") == 0)
            result = false;
    }
    sqlite3_finalize(statement);
    return result;
}

void Database::SaveLastConnectedPaper(bool paper) {
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        db_,
        "INSERT INTO app_settings(key,value) VALUES('last_connected_account',?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, paper ? "paper" : "live", -1,
                      SQLITE_STATIC);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

void Database::ClearLastConnectedAccount(bool paper) {
    const std::optional<bool> previous = LoadLastConnectedPaper();
    if (!previous || *previous != paper) return;
    std::scoped_lock lock(db_mutex_);
    Execute("DELETE FROM app_settings WHERE key='last_connected_account'");
}

std::string Database::LoadAccountAlias(const std::string& account_id) {
    std::string result;
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        db_, "SELECT alias FROM account_aliases WHERE account_id=?", -1,
        &statement, nullptr);
    sqlite3_bind_text(statement, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const char* alias = reinterpret_cast<const char*>(
            sqlite3_column_text(statement, 0));
        if (alias) result = alias;
    }
    sqlite3_finalize(statement);
    return result;
}

void Database::SaveAccountAlias(const std::string& account_id,
                                const std::string& account_number,
                                const std::string& alias) {
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        db_,
        "INSERT INTO account_aliases(account_id,account_number,alias,"
        "updated_at_ms) VALUES(?,?,?,?) "
        "ON CONFLICT(account_id) DO UPDATE SET "
        "account_number=excluded.account_number,alias=excluded.alias,"
        "updated_at_ms=excluded.updated_at_ms",
        -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, account_number.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, alias.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 4, NowMs());
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

WindowPlacement Database::LoadWindowPlacement() {
    WindowPlacement result;
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        db_,
        "SELECT x,y,width,height,maximized FROM window_placement WHERE id=1",
        -1, &statement, nullptr);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        result.x = sqlite3_column_int(statement, 0);
        result.y = sqlite3_column_int(statement, 1);
        result.width = sqlite3_column_int(statement, 2);
        result.height = sqlite3_column_int(statement, 3);
        result.maximized = sqlite3_column_int(statement, 4) != 0;
        result.exists = true;
    }
    sqlite3_finalize(statement);
    return result;
}

void Database::SaveWindowPlacement(const WindowPlacement& placement) {
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        db_,
        "INSERT INTO window_placement(id,x,y,width,height,maximized) "
        "VALUES(1,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
        "x=excluded.x,y=excluded.y,width=excluded.width,height=excluded.height,"
        "maximized=excluded.maximized",
        -1, &statement, nullptr);
    sqlite3_bind_int(statement, 1, placement.x);
    sqlite3_bind_int(statement, 2, placement.y);
    sqlite3_bind_int(statement, 3, placement.width);
    sqlite3_bind_int(statement, 4, placement.height);
    sqlite3_bind_int(statement, 5, placement.maximized ? 1 : 0);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

std::vector<Bar> Database::LoadBars(const std::string& symbol) {
    std::vector<Bar> result;
    std::scoped_lock lock(db_mutex_);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        db_,
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
    Execute("BEGIN IMMEDIATE;");
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        db_,
        "INSERT INTO daily_bars(symbol,timestamp_ms,open,high,low,close,volume,feed)"
        " VALUES(?,?,?,?,?,?,?,'iex') ON CONFLICT(symbol,timestamp_ms,feed) DO "
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
    Execute("COMMIT;");
}

void Database::QueueTimelineEvent(std::string source,
                                  std::string source_event_id,
                                  std::string kind, std::string symbol,
                                  std::int64_t event_time_ms,
                                  std::string payload) {
    {
        std::scoped_lock lock(queue_mutex_);
        pending_.push_back(
            {std::move(source), std::move(source_event_id), std::move(kind),
             std::move(symbol), event_time_ms, NowMs(), std::move(payload)});
    }
    queue_cv_.notify_one();
}

void Database::WriterLoop() {
    for (;;) {
        std::vector<PendingEvent> batch;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                return stopping_ || !pending_.empty();
            });
            while (!pending_.empty() && batch.size() < 1000) {
                batch.push_back(std::move(pending_.front()));
                pending_.pop_front();
            }
            if (batch.empty() && stopping_) break;
        }
        FlushEvents(batch);
    }
    std::vector<PendingEvent> remaining;
    {
        std::scoped_lock lock(queue_mutex_);
        while (!pending_.empty()) {
            remaining.push_back(std::move(pending_.front()));
            pending_.pop_front();
        }
    }
    FlushEvents(remaining);
}

void Database::FlushEvents(std::vector<PendingEvent>& events) {
    if (events.empty()) return;
    std::scoped_lock lock(db_mutex_);
    Execute("BEGIN IMMEDIATE;");
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        db_,
        "INSERT OR IGNORE INTO timeline_events("
        "source,source_event_id,kind,symbol,event_time_ms,available_at_ms,"
        "recorded_at_ms,payload_json,schema_version) VALUES(?,?,?,?,?,?,?,?,1)",
        -1, &statement, nullptr);
    for (const PendingEvent& event : events) {
        sqlite3_bind_text(statement, 1, event.source.c_str(), -1,
                          SQLITE_TRANSIENT);
        if (event.source_event_id.empty())
            sqlite3_bind_null(statement, 2);
        else
            sqlite3_bind_text(statement, 2, event.source_event_id.c_str(), -1,
                              SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, event.kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 4, event.symbol.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 5, event.event_time_ms);
        sqlite3_bind_int64(statement, 6, event.available_at_ms);
        sqlite3_bind_int64(statement, 7, NowMs());
        sqlite3_bind_text(statement, 8, event.payload.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_step(statement);
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);
    Execute("COMMIT;");
}
