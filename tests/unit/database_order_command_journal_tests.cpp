#include "tradebox/persistence/database_order_command_journal.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

using namespace tradebox;

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        const auto unique =
            std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
                    ("tradebox-order-journal-" +
                     std::to_string(unique));
        path = directory / "test.db";
    }

    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    std::filesystem::path directory;
    std::filesystem::path path;
};

core::NativeOrderCommand Command() {
    return core::PlaceOrderCommand{
        .context =
            {
                .request_id = "durable-request",
                .source = "integration-test",
                .account_id = "account-1",
                .environment = core::AccountEnvironment::Paper,
                .generation = core::ConnectionGeneration{7},
            },
        .order =
            {
                .asset_class = core::AssetClass::Equity,
                .symbol = "AAPL",
                .qty = *core::Decimal::Parse("0.123456789"),
                .side = core::OrderSide::Buy,
                .type = core::OrderType::Market,
                .time_in_force = core::TimeInForce::Day,
                .client_order_id = "tb-durable-request",
            },
    };
}

core::OrderCommandRecord Record() {
    return {
        .request_id = "durable-request",
        .source = "integration-test",
        .kind = core::OrderCommandKind::Place,
        .account_id = "account-1",
        .environment = core::AccountEnvironment::Paper,
        .generation = core::ConnectionGeneration{7},
        .client_order_id = "tb-durable-request",
        .created_at_ms = 1234,
    };
}

TEST(DatabaseOrderCommandJournal,
     PersistsExactPayloadAndTerminalResultAcrossReopen) {
    TemporaryDatabase temporary;
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        persistence::DatabaseOrderCommandJournal journal(database);

        const auto reserved = journal.Reserve(Record(), Command());
        ASSERT_TRUE(reserved);
        EXPECT_EQ(reserved->reservation,
                  core::CommandReservation::Reserved);
        ASSERT_TRUE(journal.Complete({
            .request_id = "durable-request",
            .outcome = core::OrderCommandOutcome::BrokerAccepted,
            .broker_order_id = "broker-1",
            .http_status = 200,
            .message = "accepted",
            .raw_response = R"({"id":"broker-1"})",
            .items = {{
                .id = "broker-1",
                .symbol = "AAPL",
                .http_status = 200,
                .accepted = true,
                .message = "accepted",
                .raw_response = R"({"id":"broker-1"})",
            }},
            .reconciliation_required = true,
        }));

        sqlite3* inspection = nullptr;
        ASSERT_EQ(sqlite3_open_v2(
                      temporary.path.string().c_str(), &inspection,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr),
                  SQLITE_OK);
        sqlite3_stmt* statement = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(
                      inspection,
                      "SELECT payload_json FROM order_commands "
                      "WHERE request_id='durable-request'",
                      -1, &statement, nullptr),
                  SQLITE_OK);
        ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
        const auto* payload = sqlite3_column_text(statement, 0);
        ASSERT_NE(payload, nullptr);
        EXPECT_NE(std::string(
                      reinterpret_cast<const char*>(payload))
                      .find("0.123456789"),
                  std::string::npos);
        sqlite3_finalize(statement);
        sqlite3_close(inspection);
    }
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        persistence::DatabaseOrderCommandJournal journal(database);

        const auto duplicate = journal.Reserve(Record(), Command());
        ASSERT_TRUE(duplicate);
        EXPECT_EQ(duplicate->reservation,
                  core::CommandReservation::Duplicate);
        ASSERT_TRUE(duplicate->existing_result);
        EXPECT_EQ(duplicate->existing_result->outcome,
                  core::OrderCommandOutcome::BrokerAccepted);
        EXPECT_EQ(duplicate->existing_result->broker_order_id,
                  "broker-1");
        ASSERT_EQ(duplicate->existing_result->items.size(), 1U);
        EXPECT_EQ(duplicate->existing_result->items[0].symbol, "AAPL");
        EXPECT_TRUE(
            duplicate->existing_result->reconciliation_required);
        const auto lookup = journal.Lookup("durable-request");
        ASSERT_TRUE(lookup);
        EXPECT_TRUE(lookup->exists);
        ASSERT_TRUE(lookup->terminal_result);
        EXPECT_EQ(lookup->terminal_result->http_status, 200U);
        ASSERT_EQ(lookup->terminal_result->items.size(), 1U);
        EXPECT_TRUE(lookup->terminal_result->items[0].accepted);
        EXPECT_TRUE(
            lookup->terminal_result->reconciliation_required);
    }
}

TEST(DatabaseOrderCommandJournal,
     PersistsDispatchAndResolvesIndeterminateRecoveryAcrossReopen) {
    TemporaryDatabase temporary;
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        persistence::DatabaseOrderCommandJournal journal(database);
        ASSERT_TRUE(journal.Reserve(Record(), Command()));
        ASSERT_TRUE(
            journal.MarkDispatchStarted("durable-request"));
        ASSERT_TRUE(journal.Complete({
            .request_id = "durable-request",
            .outcome = core::OrderCommandOutcome::Indeterminate,
            .message = "connection lost after dispatch",
            .reconciliation_required = true,
            .recovery_state =
                core::CommandRecoveryState::Pending,
            .recovery_message =
                "Awaiting authoritative broker state",
        }));
    }
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        persistence::DatabaseOrderCommandJournal journal(database);

        const auto pending = journal.Recoverable();
        ASSERT_TRUE(pending);
        ASSERT_EQ(pending->size(), 1U);
        EXPECT_TRUE(pending->front().dispatch_started);
        EXPECT_EQ(
            pending->front().record.client_order_id,
            "tb-durable-request");

        ASSERT_TRUE(journal.ResolveRecovery({
            .request_id = "durable-request",
            .outcome = core::OrderCommandOutcome::BrokerAccepted,
            .broker_order_id = "broker-recovered",
            .message = "resolved by client_order_id",
            .recovery_state =
                core::CommandRecoveryState::Resolved,
            .recovery_message =
                "Matched authoritative broker order",
        }));
        const auto lookup = journal.Lookup("durable-request");
        ASSERT_TRUE(lookup);
        ASSERT_TRUE(lookup->terminal_result);
        EXPECT_EQ(
            lookup->terminal_result->recovery_state,
            core::CommandRecoveryState::Resolved);
        EXPECT_EQ(
            lookup->terminal_result->broker_order_id,
            "broker-recovered");
        const auto none = journal.Recoverable();
        ASSERT_TRUE(none);
        EXPECT_TRUE(none->empty());
    }
}

TEST(DatabaseSettings, PersistsUserInterfacePerformanceOptions) {
    TemporaryDatabase temporary;
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        EXPECT_FALSE(database.LoadAppSetting("ui.vsync"));
        database.SaveAppSetting("ui.vsync", "1");
        database.SaveAppSetting("ui.maximum_fps", "240");
        database.SaveAppSetting("ui.maximum_fps", "120");
    }
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        EXPECT_EQ(database.LoadAppSetting("ui.vsync"), "1");
        EXPECT_EQ(database.LoadAppSetting("ui.maximum_fps"), "120");
    }
}

TEST(DatabaseAccountActivities,
     DeduplicatesRevisesFiltersPagesAndLoadsAcrossRestart) {
    TemporaryDatabase temporary;
    core::AccountActivity fill{
        .account_id = "account-1",
        .provider_id = "fill-1",
        .activity_type = "FILL",
        .execution_type = "fill",
        .symbol = "AAPL",
        .order_id = "order-1",
        .occurred_at = "2026-07-29T12:00:00Z",
        .occurred_at_ms = 1'753'790'400'000,
        .qty = *core::Decimal::Parse("1.25"),
        .price = *core::Decimal::Parse("201.123456789"),
        .fill_reconciliation =
            core::ActivityFillReconciliation::MatchedOrder,
        .raw_payload = R"({"id":"fill-1","price":"201.123456789"})",
    };
    core::AccountActivity dividend{
        .account_id = "account-1",
        .provider_id = "div-1",
        .activity_type = "DIV",
        .activity_subtype = "CDIV",
        .symbol = "MSFT",
        .currency = "USD",
        .occurred_at = "2026-07-29T13:00:00Z",
        .occurred_at_ms = 1'753'794'000'000,
        .net_amount = *core::Decimal::Parse("12.34"),
        .raw_payload = R"({"id":"div-1","net_amount":"12.34"})",
    };
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        auto first =
            database.StoreAccountActivities({fill, dividend});
        ASSERT_TRUE(first);
        EXPECT_EQ(first->inserted, 2U);
        auto duplicate =
            database.StoreAccountActivities({fill});
        ASSERT_TRUE(duplicate);
        EXPECT_EQ(duplicate->unchanged, 1U);

        fill.price = *core::Decimal::Parse("201.25");
        fill.execution_type = "trade_correct";
        fill.raw_payload =
            R"({"id":"fill-1","price":"201.25","type":"trade_correct"})";
        auto correction =
            database.StoreAccountActivities({fill});
        ASSERT_TRUE(correction);
        EXPECT_EQ(correction->revised, 1U);

        const auto first_page = database.LoadAccountActivities({
            .account_id = "account-1",
            .maximum = 1,
        });
        ASSERT_EQ(first_page.activities.size(), 1U);
        EXPECT_EQ(first_page.activities[0].provider_id, "div-1");
        EXPECT_FALSE(first_page.next_cursor_provider_id.empty());
        const auto second_page = database.LoadAccountActivities({
            .account_id = "account-1",
            .cursor_time_ms = first_page.next_cursor_time_ms,
            .cursor_provider_id =
                first_page.next_cursor_provider_id,
            .maximum = 1,
        });
        ASSERT_EQ(second_page.activities.size(), 1U);
        EXPECT_EQ(second_page.activities[0].provider_id, "fill-1");
    }
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        const auto fills = database.LoadAccountActivities({
            .account_id = "account-1",
            .activity_type = "FILL",
            .symbol = "AAPL",
        });
        ASSERT_EQ(fills.activities.size(), 1U);
        EXPECT_EQ(fills.activities[0].price->ToString(), "201.25");
        EXPECT_EQ(fills.activities[0].execution_type, "trade_correct");
        EXPECT_EQ(fills.activities[0].revision, 2U);
        EXPECT_EQ(
            fills.activities[0].fill_reconciliation,
            core::ActivityFillReconciliation::MatchedOrder);
    }
}

TEST(DatabaseMarketData, RetainsRawTicksAndReportsStorageByCategory) {
    TemporaryDatabase temporary;
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        database.StoreBars(
            "AMD", {{1'700'000'000'000, 100.0, 102.0, 99.0, 101.0,
                     1'000'000.0}});
        ASSERT_TRUE(database.QueueMarketTickEvent(
            "sip", "AMD:trade-1", "t", "alpaca:asset-amd", "AMD",
            1'700'000'000'123'456'789LL, 1'700'000'000'124,
            R"({"T":"t","S":"AMD","i":1,"p":101.125,"s":10})"));
        ASSERT_TRUE(database.QueueMarketTickEvent(
            "sip", "AMD:trade-1", "t", "alpaca:asset-amd", "AMD",
            1'700'000'000'123'456'789LL, 1'700'000'000'124,
            R"({"duplicate":true})"));
        ASSERT_TRUE(database.QueueMarketTickEvent(
            "sip", "", "q", "alpaca:asset-amd", "AMD",
            1'700'000'000'223'456'789LL,
            1'700'000'000'224,
            R"({"T":"q","S":"AMD","bp":101.12,"ap":101.13})"));
        ASSERT_TRUE(database.QueueMarketTickEvent(
            "sip", "", "q", "alpaca:asset-amd", "AMD",
            1'700'000'000'323'456'789LL,
            1'700'000'000'324,
            R"({"T":"q","S":"AMD","bp":101.13,"ap":101.14})"));
    }

    Database reopened;
    std::string error;
    ASSERT_TRUE(reopened.OpenAt(temporary.path, error)) << error;
    const MarketDataStorageUsage usage =
        reopened.LoadMarketDataStorageUsage();
    EXPECT_EQ(usage.candlestick_rows, 1U);
    EXPECT_EQ(usage.tick_rows, 3U);
    EXPECT_GT(usage.candlestick_bytes, 0U);
    EXPECT_GT(usage.tick_bytes, 0U);
    EXPECT_GE(usage.database_bytes,
              usage.candlestick_bytes + usage.tick_bytes);

    sqlite3* inspection = nullptr;
    const std::filesystem::path market_path =
        temporary.directory / "market_data.db";
    ASSERT_EQ(sqlite3_open_v2(
                  market_path.string().c_str(), &inspection,
                  SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr),
              SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
                  inspection,
                  "SELECT raw_payload FROM market_tick_events "
                  "WHERE source_event_id='AMD:trade-1'",
                  -1, &statement, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    const auto* raw_payload = sqlite3_column_text(statement, 0);
    ASSERT_NE(raw_payload, nullptr);
    EXPECT_EQ(std::string(
                  reinterpret_cast<const char*>(raw_payload)),
              R"({"T":"t","S":"AMD","i":1,"p":101.125,"s":10})");
    sqlite3_finalize(statement);
    sqlite3_close(inspection);
}

TEST(DatabaseMarketData,
     TracksCoverageAndReconstructsCorrectedFeedSeparatedTicks) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    const core::TickQuery query{
        .instrument_id = "alpaca:asset-amd",
        .symbol = "AMD",
        .start_ns = 100,
        .end_ns = 500,
        .feed = core::MarketDataFeed::Iex,
        .include_trades = true,
        .include_quotes = true,
    };
    ASSERT_TRUE(database.MarkMarketTickCoverage(query, "t", {100, 200}));
    ASSERT_TRUE(database.MarkMarketTickCoverage(query, "t", {300, 400}));
    auto missing = database.MissingMarketTickCoverage(query, "t");
    ASSERT_EQ(missing.size(), 2U);
    EXPECT_EQ(missing[0].start_ns, 200);
    EXPECT_EQ(missing[0].end_ns, 300);
    EXPECT_EQ(missing[1].start_ns, 400);
    EXPECT_EQ(missing[1].end_ns, 500);
    ASSERT_TRUE(database.MarkMarketTickCoverage(query, "t", missing[0]));
    ASSERT_TRUE(database.MarkMarketTickCoverage(query, "t", missing[1]));
    EXPECT_TRUE(
        database.MissingMarketTickCoverage(query, "t").empty());

    ASSERT_TRUE(database.StoreMarketTickEvents({
        {"iex", "AMD:1", "t", "alpaca:asset-amd", "AMD", 110, 1,
         R"({"T":"t","S":"AMD","i":1,"p":100.125,"s":2,"x":"V","t":"a"})"},
        {"iex", "AMD:2", "t", "alpaca:asset-amd", "AMD", 120, 2,
         R"({"T":"t","S":"AMD","i":2,"p":101.25,"s":3,"x":"K","t":"b"})"},
        {"iex", "AMD:c:1", "c", "alpaca:asset-amd", "AMD", 900, 3,
         R"({"T":"c","S":"AMD","oi":1,"ci":3,"cp":100.5,"cs":2,"x":"V","t":"c"})"},
        {"iex", "AMD:x:2", "x", "alpaca:asset-amd", "AMD", 910, 4,
         R"({"T":"x","S":"AMD","i":2,"t":"d"})"},
        {"iex", "AMD:q:150", "q", "alpaca:asset-amd", "AMD", 150, 5,
         R"({"T":"q","S":"AMD","bp":100.49,"bs":4,"ap":100.51,"as":5,"t":"e"})"},
        {"iex", "AMD:end", "t", "alpaca:asset-amd", "AMD", 500, 6,
         R"({"T":"t","S":"AMD","i":4,"p":777,"s":1,"t":"end"})"},
        {"sip", "AMD:9", "t", "alpaca:asset-amd", "AMD", 160, 7,
         R"({"T":"t","S":"AMD","i":9,"p":999,"s":1,"t":"f"})"},
    }));

    const core::TickSeries series = database.LoadMarketTicks(query);
    ASSERT_EQ(series.trades.size(), 1U);
    EXPECT_EQ(series.trades.front().trade_id, "3");
    EXPECT_EQ(series.trades.front().price.ToString(), "100.5");
    EXPECT_TRUE(series.trades.front().corrected);
    ASSERT_EQ(series.quotes.size(), 1U);
    EXPECT_EQ(series.quotes.front().bid_price.ToString(), "100.49");
    EXPECT_EQ(series.quotes.front().ask_price.ToString(), "100.51");
}

TEST(DatabaseMarketData, RetainsTicksWithNullOptionalMetadata) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    ASSERT_TRUE(database.StoreMarketTickEvents({
        {"iex", "QQQ:1", "t", "alpaca:asset-qqq", "QQQ", 110, 1,
         R"({"T":"t","S":"QQQ","i":1,"p":625.125,"s":2,"x":null,"z":null,"t":null})"},
        {"iex", "QQQ:q:120", "q", "alpaca:asset-qqq", "QQQ", 120, 2,
         R"({"T":"q","S":"QQQ","bp":625.12,"bs":4,"ap":625.13,"as":5,"bx":null,"ax":null,"z":null,"t":null})"},
    }));

    const core::TickSeries series = database.LoadMarketTicks({
        .instrument_id = "alpaca:asset-qqq",
        .symbol = "QQQ",
        .start_ns = 100,
        .end_ns = 200,
        .feed = core::MarketDataFeed::Iex,
        .include_trades = true,
        .include_quotes = true,
    });

    ASSERT_EQ(series.trades.size(), 1U);
    EXPECT_EQ(series.trades.front().price.ToString(), "625.125");
    EXPECT_TRUE(series.trades.front().exchange.empty());
    EXPECT_TRUE(series.trades.front().tape.empty());
    EXPECT_TRUE(series.trades.front().broker_timestamp.empty());
    ASSERT_EQ(series.quotes.size(), 1U);
    EXPECT_EQ(series.quotes.front().ask_price.ToString(), "625.13");
    EXPECT_TRUE(series.quotes.front().bid_exchange.empty());
    EXPECT_TRUE(series.quotes.front().ask_exchange.empty());
    EXPECT_TRUE(series.quotes.front().tape.empty());
}

TEST(DatabaseMarketData,
     SeparatesTickRowsAndCoverageByStableInstrumentIdentity) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    const core::TickQuery first{
        .instrument_id = "alpaca:first-asset",
        .symbol = "REUSED",
        .start_ns = 100,
        .end_ns = 200,
        .feed = core::MarketDataFeed::Iex,
    };
    core::TickQuery second = first;
    second.instrument_id = "alpaca:second-asset";
    ASSERT_TRUE(database.StoreMarketTickEvents({
        {"iex", "first:trade", "t", first.instrument_id, first.symbol,
         110, 1,
         R"({"T":"t","S":"REUSED","i":"same","p":10,"s":1})"},
        {"iex", "second:trade", "t", second.instrument_id, second.symbol,
         120, 2,
         R"({"T":"t","S":"REUSED","i":"same","p":20,"s":1})"},
    }));
    ASSERT_TRUE(database.MarkMarketTickCoverage(
        first, "t", {first.start_ns, first.end_ns}));

    const auto first_ticks = database.LoadMarketTicks(first);
    ASSERT_EQ(first_ticks.trades.size(), 1U);
    EXPECT_EQ(first_ticks.trades[0].price.ToString(), "10");
    EXPECT_TRUE(
        database.MissingMarketTickCoverage(first, "t").empty());

    const auto second_ticks = database.LoadMarketTicks(second);
    ASSERT_EQ(second_ticks.trades.size(), 1U);
    EXPECT_EQ(second_ticks.trades[0].price.ToString(), "20");
    const auto second_missing =
        database.MissingMarketTickCoverage(second, "t");
    ASSERT_EQ(second_missing.size(), 1U);
    EXPECT_EQ(second_missing[0].start_ns, 100);
    EXPECT_EQ(second_missing[0].end_ns, 200);
}

TEST(DatabaseMarketData,
     MigratesLegacySymbolOnlyTicksWithoutTrustingLegacyCoverage) {
    TemporaryDatabase temporary;
    std::filesystem::create_directories(temporary.directory);
    sqlite3* legacy = nullptr;
    const std::filesystem::path market_path =
        temporary.directory / "market_data.db";
    ASSERT_EQ(
        sqlite3_open_v2(
            market_path.string().c_str(), &legacy,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                SQLITE_OPEN_FULLMUTEX,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(
        sqlite3_exec(
            legacy,
            "CREATE TABLE market_tick_events("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "feed TEXT NOT NULL,source_event_id TEXT,kind TEXT NOT NULL,"
            "symbol TEXT NOT NULL,event_time_ns INTEGER NOT NULL,"
            "received_at_ms INTEGER NOT NULL,raw_payload TEXT NOT NULL,"
            "UNIQUE(feed,source_event_id,kind));"
            "CREATE INDEX market_tick_events_series ON "
            "market_tick_events(symbol,event_time_ns,feed,kind);"
            "CREATE TABLE market_tick_coverage("
            "symbol TEXT NOT NULL,feed TEXT NOT NULL,kind TEXT NOT NULL,"
            "start_ns INTEGER NOT NULL,end_ns INTEGER NOT NULL,"
            "completed_at_ms INTEGER NOT NULL,"
            "PRIMARY KEY(symbol,feed,kind,start_ns,end_ns));"
            "INSERT INTO market_tick_coverage VALUES("
            "'OLD','iex','t',100,200,1);",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
    sqlite3_close(legacy);

    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    const core::TickQuery query{
        .instrument_id = "alpaca:new-identity",
        .symbol = "OLD",
        .start_ns = 100,
        .end_ns = 200,
        .feed = core::MarketDataFeed::Iex,
    };
    const auto missing =
        database.MissingMarketTickCoverage(query, "t");
    ASSERT_EQ(missing.size(), 1U);
    EXPECT_EQ(missing[0].start_ns, 100);
    EXPECT_EQ(missing[0].end_ns, 200);
    ASSERT_TRUE(database.StoreMarketTickEvents({
        {"iex", "new:trade", "t", query.instrument_id, query.symbol,
         110, 2,
         R"({"T":"t","S":"OLD","i":"new","p":30,"s":1})"},
    }));
    const auto loaded = database.LoadMarketTicks(query);
    ASSERT_EQ(loaded.trades.size(), 1U);
    EXPECT_EQ(loaded.trades[0].price.ToString(), "30");
}

TEST(DatabaseMarketData,
     DoesNotApplyAnOutOfSessionCorrectionToARepeatedTradeId) {
    constexpr std::int64_t day_ns =
        24LL * 60 * 60 * 1'000'000'000;
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    const core::TickQuery first_day{
        .instrument_id = "alpaca:asset-repeat",
        .symbol = "REPEAT",
        .start_ns = 0,
        .end_ns = day_ns,
        .feed = core::MarketDataFeed::Sip,
    };
    ASSERT_TRUE(database.StoreMarketTickEvents({
        {"sip", "day-one", "t", first_day.instrument_id,
         first_day.symbol, 100, 1,
         R"({"T":"t","S":"REPEAT","i":"same","p":10,"s":1})"},
        {"sip", "day-two", "t", first_day.instrument_id,
         first_day.symbol, day_ns + 100, 2,
         R"({"T":"t","S":"REPEAT","i":"same","p":20,"s":1})"},
        {"sip", "day-two-correction", "c", first_day.instrument_id,
         first_day.symbol, day_ns + 200, 3,
         R"({"T":"c","S":"REPEAT","oi":"same","ci":"corrected","cp":21,"cs":1})"},
    }));

    const auto loaded = database.LoadMarketTicks(first_day);
    ASSERT_EQ(loaded.trades.size(), 1U);
    EXPECT_EQ(loaded.trades[0].trade_id, "same");
    EXPECT_EQ(loaded.trades[0].price.ToString(), "10");
    EXPECT_FALSE(loaded.trades[0].corrected);
}

TEST(DatabaseMarketData,
     SurfacesAsynchronousPersistenceFailureToFlushAndHealth) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    sqlite3* injection = nullptr;
    const std::filesystem::path market_path =
        temporary.directory / "market_data.db";
    ASSERT_EQ(
        sqlite3_open_v2(
            market_path.string().c_str(), &injection,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr),
        SQLITE_OK);
    ASSERT_EQ(
        sqlite3_exec(
            injection,
            "CREATE TRIGGER reject_typed_ticks "
            "BEFORE INSERT ON typed_market_ticks "
            "BEGIN SELECT RAISE(ABORT,'injected persistence failure'); END;",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
    sqlite3_close(injection);

    ASSERT_TRUE(database.QueueMarketDataEvent(
        "iex", "failure:trade",
        core::ShareMarketDataEvent(core::TradeReceived{
            .trade = {
                .instrument_id = "alpaca:asset-failure",
                .symbol = "FAIL",
                .trade_id = "1",
                .price = *core::Decimal::Parse("1"),
                .size = *core::Decimal::Parse("1"),
                .event_time_ns = 1,
                .received_at_ms = 1,
            },
        })));
    const auto flushed = database.FlushQueuedWrites();
    ASSERT_FALSE(flushed);
    EXPECT_NE(
        flushed.error().find("injected persistence failure"),
        std::string::npos);
    const auto health = database.WriterTelemetry();
    EXPECT_EQ(health.write_failures, 1U);
    EXPECT_NE(
        health.last_write_error.find("injected persistence failure"),
        std::string::npos);
}

TEST(DatabaseMarketData,
     PersistsExactProviderBarsCoverageAndRevisions) {
    TemporaryDatabase temporary;
    const core::BarSeriesKey key{
        .instrument_id = "alpaca:asset-qqq",
        .feed = core::MarketDataFeed::Iex,
        .timeframe = "1Min",
        .adjustment = core::BarAdjustment::Raw,
    };
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        ASSERT_TRUE(database.StoreProviderBars({
            .key = key,
            .symbol = "QQQ",
            .bars = {{
                .start_ns = 60'000'000'000,
                .open = *core::Decimal::Parse("500.01"),
                .high = *core::Decimal::Parse("500.20"),
                .low = *core::Decimal::Parse("499.99"),
                .close = *core::Decimal::Parse("500.125"),
                .volume = *core::Decimal::Parse("12345"),
                .within_bar_vwap =
                    *core::Decimal::Parse("500.075"),
                .trade_count = 321,
                .source = core::BarSource::ProviderHistorical,
                .state = core::BarState::Finalized,
            }},
            .covered_range =
                core::BarRange{60'000'000'000,
                               120'000'000'000},
        }));
        ASSERT_TRUE(database.QueueProviderBars({
            .key = key,
            .symbol = "QQQ",
            .bars = {{
                .start_ns = 60'000'000'000,
                .open = *core::Decimal::Parse("500.01"),
                .high = *core::Decimal::Parse("500.20"),
                .low = *core::Decimal::Parse("499.99"),
                .close = *core::Decimal::Parse("500.15"),
                .volume = *core::Decimal::Parse("12400"),
                .trade_count = 322,
                .source = core::BarSource::ProviderStream,
                .state = core::BarState::Finalized,
            }},
        }));
        const auto telemetry = database.WriterTelemetry();
        EXPECT_EQ(telemetry.accepted_bars, 1U);
        EXPECT_EQ(telemetry.dropped_bars, 0U);
    }

    Database reopened;
    std::string error;
    ASSERT_TRUE(reopened.OpenAt(temporary.path, error)) << error;
    const auto loaded = reopened.LoadProviderBars(
        key, {0, 180'000'000'000});
    ASSERT_EQ(loaded.bars.size(), 1U);
    EXPECT_EQ(loaded.bars[0].close.ToString(), "500.15");
    EXPECT_EQ(loaded.bars[0].volume.ToString(), "12400");
    EXPECT_EQ(loaded.bars[0].revision, 2U);
    EXPECT_EQ(loaded.bars[0].state, core::BarState::Revised);
    EXPECT_EQ(
        loaded.coverage,
        (std::vector<core::BarRange>{{
            60'000'000'000, 120'000'000'000}}));
}

TEST(DatabaseMarketData,
     LateNormalStreamBarCannotRollBackDurableRevision) {
    TemporaryDatabase temporary;
    const core::BarSeriesKey key{
        .instrument_id = "alpaca:asset-qqq",
        .feed = core::MarketDataFeed::Sip,
        .timeframe = "1Min",
        .adjustment = core::BarAdjustment::Raw,
    };
    const auto stream_bar =
        [](std::string_view close, core::BarState state) {
            return core::MarketBar{
                .start_ns = 60'000'000'000,
                .open = *core::Decimal::Parse("500"),
                .high = *core::Decimal::Parse(close),
                .low = *core::Decimal::Parse("499"),
                .close = *core::Decimal::Parse(close),
                .volume = *core::Decimal::Parse("100"),
                .trade_count = 10,
                .source = core::BarSource::ProviderStream,
                .state = state,
            };
        };
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        ASSERT_TRUE(database.StoreProviderBars({
            .key = key,
            .symbol = "QQQ",
            .bars = {
                stream_bar("501", core::BarState::Revised),
            },
        }));
        ASSERT_TRUE(database.StoreProviderBars({
            .key = key,
            .symbol = "QQQ",
            .bars = {
                stream_bar("500.5", core::BarState::Finalized),
            },
        }));
    }

    Database reopened;
    std::string error;
    ASSERT_TRUE(reopened.OpenAt(temporary.path, error)) << error;
    const auto loaded = reopened.LoadProviderBars(
        key, {0, 120'000'000'000});
    ASSERT_EQ(loaded.bars.size(), 1U);
    EXPECT_EQ(loaded.bars[0].close.ToString(), "501");
    EXPECT_EQ(loaded.bars[0].state, core::BarState::Revised);
    EXPECT_EQ(loaded.bars[0].revision, 1U);
}

TEST(DatabaseMarketData,
     ProviderOpenBarReopensDurableCurrentInterval) {
    TemporaryDatabase temporary;
    const core::BarSeriesKey key{
        .instrument_id = "alpaca:asset-qqq",
        .feed = core::MarketDataFeed::Iex,
        .timeframe = "1Day",
        .adjustment = core::BarAdjustment::Raw,
    };
    const auto bar = [](std::string_view close,
                        core::BarSource source,
                        core::BarState state) {
        return core::MarketBar{
            .start_ns = 60'000'000'000,
            .open = *core::Decimal::Parse("500"),
            .high = *core::Decimal::Parse("501"),
            .low = *core::Decimal::Parse("499"),
            .close = *core::Decimal::Parse(close),
            .volume = *core::Decimal::Parse("100"),
            .trade_count = 10,
            .source = source,
            .state = state,
        };
    };
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        ASSERT_TRUE(database.StoreProviderBars({
            .key = key,
            .symbol = "QQQ",
            .bars = {bar(
                "500", core::BarSource::ProviderHistorical,
                core::BarState::Finalized)},
        }));
        ASSERT_TRUE(database.StoreProviderBars({
            .key = key,
            .symbol = "QQQ",
            .bars = {bar(
                "500.5", core::BarSource::ProviderStream,
                core::BarState::Open)},
        }));
    }

    Database reopened;
    std::string error;
    ASSERT_TRUE(reopened.OpenAt(temporary.path, error)) << error;
    const auto loaded = reopened.LoadProviderBars(
        key, {0, 120'000'000'000});
    ASSERT_EQ(loaded.bars.size(), 1U);
    EXPECT_EQ(loaded.bars[0].close.ToString(), "500.5");
    EXPECT_EQ(loaded.bars[0].state, core::BarState::Open);
    EXPECT_EQ(loaded.bars[0].revision, 2U);
}

TEST(DatabaseMarketData,
     PersistsSharedTypedTicksWithoutRawJsonRoundTrip) {
    TemporaryDatabase temporary;
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        ASSERT_TRUE(database.QueueMarketDataEvent(
            "sip", "trade:typed-1",
            core::ShareMarketDataEvent(core::TradeReceived{
                .trade = {
                    .instrument_id = "alpaca:asset-aapl",
                    .symbol = "AAPL",
                    .trade_id = "typed-1",
                    .price = *core::Decimal::Parse("201.123456"),
                    .size = *core::Decimal::Parse("125.5"),
                    .exchange = "V",
                    .conditions = {"@", "I"},
                    .tape = "C",
                    .broker_timestamp =
                        "2026-07-28T14:30:00.000000001Z",
                    .event_time_ns = 100,
                    .received_at_ms = 200,
                },
            })));
        ASSERT_TRUE(database.QueueMarketDataEvent(
            "sip", "quote:typed-1",
            core::ShareMarketDataEvent(core::QuoteReceived{
                .quote = {
                    .instrument_id = "alpaca:asset-aapl",
                    .symbol = "AAPL",
                    .bid_price =
                        *core::Decimal::Parse("201.1234"),
                    .bid_size = *core::Decimal::Parse("10"),
                    .bid_exchange = "Q",
                    .ask_price =
                        *core::Decimal::Parse("201.1235"),
                    .ask_size = *core::Decimal::Parse("12"),
                    .ask_exchange = "P",
                    .conditions = {"R"},
                    .tape = "C",
                    .broker_timestamp =
                        "2026-07-28T14:30:00.000000002Z",
                    .event_time_ns = 101,
                    .received_at_ms = 201,
                },
            })));
    }

    Database reopened;
    std::string error;
    ASSERT_TRUE(reopened.OpenAt(temporary.path, error)) << error;
    const auto loaded = reopened.LoadMarketTicks({
        .instrument_id = "alpaca:asset-aapl",
        .symbol = "AAPL",
        .start_ns = 0,
        .end_ns = 1'000,
        .feed = core::MarketDataFeed::Sip,
        .include_trades = true,
        .include_quotes = true,
    });
    ASSERT_EQ(loaded.trades.size(), 1U);
    EXPECT_EQ(loaded.trades[0].instrument_id,
              "alpaca:asset-aapl");
    EXPECT_EQ(loaded.trades[0].price.ToString(),
              "201.123456");
    EXPECT_EQ(loaded.trades[0].size.ToString(), "125.5");
    EXPECT_EQ(loaded.trades[0].conditions,
              (std::vector<std::string>{"@", "I"}));
    ASSERT_EQ(loaded.quotes.size(), 1U);
    EXPECT_EQ(loaded.quotes[0].bid_price.ToString(),
              "201.1234");
    EXPECT_EQ(loaded.quotes[0].ask_price.ToString(),
              "201.1235");
    EXPECT_EQ(loaded.quotes[0].conditions,
              (std::vector<std::string>{"R"}));
}

TEST(DatabaseMarketData,
     TypedTickBatchIsValidatedAtomicallyAndPersistsInOrder) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;

    const auto trade = [](std::string id,
                          std::int64_t event_time_ns) {
        return core::ShareMarketDataEvent(core::TradeReceived{
            .trade = {
                .instrument_id = "alpaca:asset-aapl",
                .symbol = "AAPL",
                .trade_id = std::move(id),
                .price = *core::Decimal::Parse("201.25"),
                .size = *core::Decimal::Parse("10"),
                .broker_timestamp =
                    "2026-07-28T14:30:00.000000001Z",
                .event_time_ns = event_time_ns,
                .received_at_ms = 200,
            },
        });
    };

    EXPECT_FALSE(database.QueueMarketDataEvents(
        "sip",
        {
            {.source_event_id = "not-queued",
             .event = trade("not-queued", 99)},
            {.source_event_id = "invalid", .event = {}},
        }));
    EXPECT_EQ(database.WriterTelemetry().accepted_events, 0U);

    ASSERT_TRUE(database.QueueMarketDataEvents(
        "sip",
        {
            {.source_event_id = "trade:batch-1",
             .event = trade("batch-1", 100)},
            {.source_event_id = "trade:batch-2",
             .event = trade("batch-2", 101)},
        }));
    ASSERT_TRUE(database.FlushQueuedWrites());

    const auto loaded = database.LoadMarketTicks({
        .instrument_id = "alpaca:asset-aapl",
        .symbol = "AAPL",
        .start_ns = 0,
        .end_ns = 1'000,
        .feed = core::MarketDataFeed::Sip,
        .include_trades = true,
    });
    ASSERT_EQ(loaded.trades.size(), 2U);
    EXPECT_EQ(loaded.trades[0].trade_id, "batch-1");
    EXPECT_EQ(loaded.trades[1].trade_id, "batch-2");
}

TEST(DatabaseMarketData,
     RejectsAnOversizedLiveBarBatchWithoutBlocking) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;

    core::BarUpsertBatch oversized{
        .key = {
            .instrument_id = "alpaca:oversized",
            .feed = core::MarketDataFeed::Sip,
            .timeframe = "1Min",
            .adjustment = core::BarAdjustment::Raw,
        },
        .symbol = "LOAD",
    };
    oversized.bars.resize(50'001);

    EXPECT_FALSE(database.QueueProviderBars(std::move(oversized)));
    const auto telemetry = database.WriterTelemetry();
    EXPECT_EQ(telemetry.accepted_bars, 0U);
    EXPECT_EQ(telemetry.dropped_bars, 50'001U);
    EXPECT_EQ(telemetry.pending_bars, 0U);
}

}  // namespace
