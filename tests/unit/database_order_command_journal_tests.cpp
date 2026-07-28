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

}  // namespace
