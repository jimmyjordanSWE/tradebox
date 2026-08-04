#include "tradebox/persistence/database.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace tradebox;

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        const auto unique =
            std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
                    ("tradebox-watchlist-" + std::to_string(unique));
        path = directory / "test.db";
    }

    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    std::filesystem::path directory;
    std::filesystem::path path;
};

TEST(DatabaseWatchlist, PreservesOrderAcrossReopen) {
    TemporaryDatabase temporary;
    const std::vector<std::string> expected = {
        "QQQ", "AAPL", "MSFT", "AMD",
    };
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        ASSERT_TRUE(database.SaveWatchlist(expected));
        EXPECT_EQ(database.LoadWatchlist(), expected);
    }
    {
        Database database;
        std::string error;
        ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
        EXPECT_EQ(database.LoadWatchlist(), expected);
    }
}

TEST(DatabaseWatchlist, SaveFailureIsReturnedAndPreviousRowsRemain) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    const std::vector<std::string> expected = {"AAPL", "MSFT"};
    ASSERT_TRUE(database.SaveWatchlist(expected));

    const auto failed = database.SaveWatchlist({"AAPL", "AAPL"});
    ASSERT_FALSE(failed);
    EXPECT_FALSE(failed.error().empty());
    EXPECT_EQ(database.LoadWatchlist(), expected);
}

TEST(DatabaseWatchlist, SaveFailureIsVisibleWhenDatabaseIsClosed) {
    Database database;
    const auto failed = database.SaveWatchlist({"AAPL"});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error(), "Database is not open");
}

}  // namespace
