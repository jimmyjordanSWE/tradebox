#include "tradebox/persistence/database_order_command_journal.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <string>

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
        const auto lookup = journal.Lookup("durable-request");
        ASSERT_TRUE(lookup);
        EXPECT_TRUE(lookup->exists);
        ASSERT_TRUE(lookup->terminal_result);
        EXPECT_EQ(lookup->terminal_result->http_status, 200U);
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

TEST(DatabaseMarketData, RetainsRawTicksAndReportsStorageByCategory) {
    TemporaryDatabase temporary;
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        database.StoreBars(
            "AMD", {{1'700'000'000'000, 100.0, 102.0, 99.0, 101.0,
                     1'000'000.0}});
        database.QueueMarketTickEvent(
            "sip", "AMD:trade-1", "t", "AMD",
            1'700'000'000'123'456'789LL, 1'700'000'000'124,
            R"({"T":"t","S":"AMD","i":1,"p":101.125,"s":10})");
        database.QueueMarketTickEvent(
            "sip", "AMD:trade-1", "t", "AMD",
            1'700'000'000'123'456'789LL, 1'700'000'000'124,
            R"({"duplicate":true})");
        database.QueueMarketTickEvent(
            "sip", "", "q", "AMD", 1'700'000'000'223'456'789LL,
            1'700'000'000'224,
            R"({"T":"q","S":"AMD","bp":101.12,"ap":101.13})");
        database.QueueMarketTickEvent(
            "sip", "", "q", "AMD", 1'700'000'000'323'456'789LL,
            1'700'000'000'324,
            R"({"T":"q","S":"AMD","bp":101.13,"ap":101.14})");
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
        .symbol = "AMD",
        .start_ns = 100,
        .end_ns = 500,
        .feed = core::MarketDataFeed::Iex,
        .include_trades = true,
        .include_quotes = true,
    };
    database.MarkMarketTickCoverage(query, "t", {100, 200});
    database.MarkMarketTickCoverage(query, "t", {300, 400});
    auto missing = database.MissingMarketTickCoverage(query, "t");
    ASSERT_EQ(missing.size(), 2U);
    EXPECT_EQ(missing[0].start_ns, 200);
    EXPECT_EQ(missing[0].end_ns, 300);
    EXPECT_EQ(missing[1].start_ns, 400);
    EXPECT_EQ(missing[1].end_ns, 500);
    database.MarkMarketTickCoverage(query, "t", missing[0]);
    database.MarkMarketTickCoverage(query, "t", missing[1]);
    EXPECT_TRUE(
        database.MissingMarketTickCoverage(query, "t").empty());

    database.StoreMarketTickEvents({
        {"iex", "AMD:1", "t", "AMD", 110, 1,
         R"({"T":"t","S":"AMD","i":1,"p":100.125,"s":2,"x":"V","t":"a"})"},
        {"iex", "AMD:2", "t", "AMD", 120, 2,
         R"({"T":"t","S":"AMD","i":2,"p":101.25,"s":3,"x":"K","t":"b"})"},
        {"iex", "AMD:c:1", "c", "AMD", 900, 3,
         R"({"T":"c","S":"AMD","oi":1,"ci":3,"cp":100.5,"cs":2,"x":"V","t":"c"})"},
        {"iex", "AMD:x:2", "x", "AMD", 910, 4,
         R"({"T":"x","S":"AMD","i":2,"t":"d"})"},
        {"iex", "AMD:q:150", "q", "AMD", 150, 5,
         R"({"T":"q","S":"AMD","bp":100.49,"bs":4,"ap":100.51,"as":5,"t":"e"})"},
        {"sip", "AMD:9", "t", "AMD", 160, 6,
         R"({"T":"t","S":"AMD","i":9,"p":999,"s":1,"t":"f"})"},
    });

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
    database.StoreMarketTickEvents({
        {"iex", "QQQ:1", "t", "QQQ", 110, 1,
         R"({"T":"t","S":"QQQ","i":1,"p":625.125,"s":2,"x":null,"z":null,"t":null})"},
        {"iex", "QQQ:q:120", "q", "QQQ", 120, 2,
         R"({"T":"q","S":"QQQ","bp":625.12,"bs":4,"ap":625.13,"as":5,"bx":null,"ax":null,"z":null,"t":null})"},
    });

    const core::TickSeries series = database.LoadMarketTicks({
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
        database.StoreProviderBars({
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
        });
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
     PersistsSharedTypedTicksWithoutRawJsonRoundTrip) {
    TemporaryDatabase temporary;
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        database.QueueMarketDataEvent(
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
            }));
        database.QueueMarketDataEvent(
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
            }));
    }

    Database reopened;
    std::string error;
    ASSERT_TRUE(reopened.OpenAt(temporary.path, error)) << error;
    const auto loaded = reopened.LoadMarketTicks({
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
