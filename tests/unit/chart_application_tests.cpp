#include "tradebox/application/chart_query.h"
#include "tradebox/application/history_request_tracker.h"
#include "tradebox/application/trading_application.h"
#include "tradebox/persistence/database.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

namespace {

tradebox::core::BarSeriesKey Key(std::string timeframe = "1Min") {
    return {
        .instrument_id = "asset-aapl",
        .feed = tradebox::core::MarketDataFeed::Iex,
        .timeframe = std::move(timeframe),
        .adjustment = tradebox::core::BarAdjustment::All,
    };
}

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        const auto unique =
            std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
                    ("tradebox-chart-application-" +
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

TEST(ChartRangePolicyTest, ResolvesDeterministicBoundedRange) {
    constexpr std::int64_t minute_ns = 60LL * 1'000'000'000;
    const auto result = tradebox::application::ResolveChartRange({
        .document_id = "chart-1",
        .key = Key(),
        .symbol = "AAPL",
        .anchor_ns = 1'000 * minute_ns,
        .visible_bars = 100,
    });

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->start_ns, 700 * minute_ns);
    EXPECT_EQ(result->end_ns, 1'001 * minute_ns);
}

TEST(ChartRangePolicyTest, RejectsUnresolvedIdentityAndTimeframe) {
    auto intent = tradebox::application::ChartViewportIntent{
        .document_id = "chart-1",
        .key = Key("unsupported"),
        .symbol = "AAPL",
        .anchor_ns = 1'000'000'000'000,
    };
    EXPECT_FALSE(tradebox::application::ResolveChartRange(intent));
    intent.key = Key();
    intent.key.instrument_id.clear();
    EXPECT_FALSE(tradebox::application::ResolveChartRange(intent));
}

TEST(HistoryRequestTrackerTest, ReportsStatusForOverlappingQuery) {
    tradebox::application::HistoryRequestTracker tracker;
    tracker.Publish({
        .key = Key(),
        .range = {100, 200},
        .state = tradebox::broker::HistoryRequestState::Loading,
    });

    const auto loading = tracker.StatusFor(Key(), {150, 250});
    ASSERT_TRUE(loading);
    EXPECT_EQ(loading->state,
              tradebox::broker::HistoryRequestState::Loading);

    tracker.Publish({
        .key = Key(),
        .range = {100, 200},
        .state = tradebox::broker::HistoryRequestState::Failed,
        .message = "provider unavailable",
    });
    const auto failed = tracker.StatusFor(Key(), {150, 250});
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->state,
              tradebox::broker::HistoryRequestState::Failed);
    EXPECT_EQ(failed->message, "provider unavailable");
    EXPECT_FALSE(tracker.StatusFor(Key(), {200, 300}));

    tracker.Publish({
        .key = Key(),
        .range = {125, 225},
        .state = tradebox::broker::HistoryRequestState::Succeeded,
    });
    const auto recovered = tracker.StatusFor(Key(), {150, 175});
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->state,
              tradebox::broker::HistoryRequestState::Succeeded);
}

TEST(ChartApplicationSnapshotTest, ExposesExplicitStatesAndAssetSearch) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    database.SaveAssetCatalog({
        {.symbol = "AAPL",
         .name = "Apple Inc.",
         .exchange = "NASDAQ",
         .active = true,
         .tradable = true,
         .instrument_id = "asset-aapl"},
    });
    tradebox::application::TradingApplication application(database);
    const tradebox::application::UiChartQuery missing{
        .document_id = "chart-1",
        .key = Key(),
        .symbol = "AAPL",
        .range = {0, 100},
    };

    auto snapshot = application.SnapshotForUi({
        .charts = {missing},
        .asset_search = "AAP",
        .asset_limit = 5,
    });
    ASSERT_EQ(snapshot.charts.size(), 1U);
    EXPECT_EQ(snapshot.charts.front().status,
              tradebox::application::ChartDataStatus::Unavailable);
    ASSERT_EQ(snapshot.assets.size(), 1U);
    EXPECT_EQ(snapshot.assets.front().instrument_id, "asset-aapl");

    application.RequestMarketHistory(missing);
    snapshot = application.SnapshotForUi({.charts = {missing}});
    EXPECT_EQ(snapshot.charts.front().status,
              tradebox::application::ChartDataStatus::Failed);
    EXPECT_TRUE(snapshot.charts.front().retryable);

    const tradebox::core::BarRange covered{1'000, 2'000};
    ASSERT_TRUE(database.StoreProviderBars({
        .key = Key(),
        .symbol = "AAPL",
        .covered_range = covered,
    }));
    const tradebox::application::UiChartQuery empty{
        .document_id = "chart-2",
        .key = Key(),
        .symbol = "AAPL",
        .range = covered,
    };
    snapshot = application.SnapshotForUi({.charts = {empty}});
    EXPECT_EQ(snapshot.charts.front().status,
              tradebox::application::ChartDataStatus::Empty);

    const auto ten = tradebox::core::Decimal::Parse("10");
    const auto volume = tradebox::core::Decimal::Parse("500");
    ASSERT_TRUE(ten && volume);
    const tradebox::core::BarRange populated{3'000, 4'000};
    ASSERT_TRUE(database.StoreProviderBars({
        .key = Key(),
        .symbol = "AAPL",
        .bars = {{.start_ns = 3'000,
                  .open = *ten,
                  .high = *ten,
                  .low = *ten,
                  .close = *ten,
                  .volume = *volume}},
        .covered_range = populated,
    }));
    const tradebox::application::UiChartQuery calculated{
        .document_id = "chart-3",
        .key = Key(),
        .symbol = "AAPL",
        .range = populated,
        .indicators = {
            {.id = "volume",
             .calculation =
                 tradebox::core::SimpleMovingAverageCalculation{
                     .input = tradebox::core::BarSeriesInput{
                         tradebox::core::BarSeriesField::Volume},
                     .period = 1}},
            {.id = "volume-again",
             .calculation =
                 tradebox::core::SimpleMovingAverageCalculation{
                     .input = tradebox::core::IndicatorOutputInput{
                         "volume", "value"},
                     .period = 1}},
        },
    };
    snapshot = application.SnapshotForUi({.charts = {calculated}});
    ASSERT_TRUE(snapshot.charts.front().indicator_projection);
    ASSERT_EQ(snapshot.charts.front().indicator_projection->series.size(), 2U);
    ASSERT_EQ(snapshot.charts.front().indicator_projection->series.back()
                  .points.size(), 1U);
    EXPECT_EQ(snapshot.charts.front().indicator_projection->series.back()
                  .points.front().value,
              *volume);
}

TEST(ChartApplicationSnapshotTest,
     AssetCatalogIsCachedAndRefreshesOnlyFromCatalogEvent) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    database.SaveAssetCatalog({
        {.symbol = "AAPL",
         .name = "Apple Inc.",
         .active = true,
         .tradable = true,
         .instrument_id = "asset-aapl"},
    });
    UiEventQueue events;
    tradebox::application::TradingApplication application(events, database);

    auto snapshot = application.SnapshotForUi({
        .asset_search = "AAP",
        .asset_limit = 5,
    });
    ASSERT_EQ(snapshot.assets.size(), 1U);
    EXPECT_EQ(snapshot.assets.front().symbol, "AAPL");

    database.SaveAssetCatalog({
        {.symbol = "MSFT",
         .name = "Microsoft Corporation",
         .active = true,
         .tradable = true,
         .instrument_id = "asset-msft"},
    });
    snapshot = application.SnapshotForUi({
        .asset_search = "MSF",
        .asset_limit = 5,
    });
    EXPECT_TRUE(snapshot.assets.empty());

    UiEvent refreshed;
    refreshed.type = UiEventType::AssetCatalogReady;
    refreshed.assets = {
        {.symbol = "MSFT",
         .name = "Microsoft Corporation",
         .active = true,
         .tradable = true,
         .instrument_id = "asset-msft"},
    };
    events.Push(std::move(refreshed));
    static_cast<void>(application.DrainUiEvents());
    snapshot = application.SnapshotForUi({
        .asset_search = "MSF",
        .asset_limit = 5,
    });
    ASSERT_EQ(snapshot.assets.size(), 1U);
    EXPECT_EQ(snapshot.assets.front().symbol, "MSFT");
}

TEST(ChartApplicationSnapshotTest, FindsStoredBarInstrumentWithoutCatalog) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    const tradebox::core::BarRange coverage{1'000, 2'000};
    const auto one = tradebox::core::Decimal::Parse("1");
    ASSERT_TRUE(one);
    ASSERT_TRUE(database.StoreProviderBars({
        .key = Key(),
        .symbol = "AAPL",
        .bars = {{.start_ns = 1'000,
                  .open = *one,
                  .high = *one,
                  .low = *one,
                  .close = *one,
                  .volume = *one}},
        .covered_range = coverage,
    }));

    tradebox::application::TradingApplication application(database);
    const auto snapshot = application.SnapshotForUi({
        .asset_search = "AAPL",
        .asset_limit = 5,
    });
    ASSERT_EQ(snapshot.assets.size(), 1U);
    EXPECT_EQ(snapshot.assets.front().symbol, "AAPL");
    EXPECT_EQ(snapshot.assets.front().instrument_id, "asset-aapl");
}

TEST(TradingApplication, RejectsPositionExitWithoutAnAccountSnapshot) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    tradebox::application::TradingApplication application(database);

    const auto result = application.ClosePosition("AAPL", true);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), "Account data is unavailable");
}

TEST(ChartApplicationSnapshotTest, PublishesOnlyUniqueRequestedMarkets) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    tradebox::application::TradingApplication application(database);

    const auto snapshot = application.SnapshotForUi({
        .market_symbols = {"AAPL", "AAPL", "MSFT"},
    });
    ASSERT_EQ(snapshot.markets.instruments.size(), 2U);
    EXPECT_NE(snapshot.markets.Find("AAPL"), nullptr);
    EXPECT_NE(snapshot.markets.Find("MSFT"), nullptr);
    EXPECT_EQ(snapshot.markets.Find("NVDA"), nullptr);
}

TEST(ChartApplicationSnapshotTest,
     WatchListResolvesStableIdentityBeforeLiveEventsArrive) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    tradebox::application::TradingApplication application(database);

    const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now());
    const auto snapshot = application.SnapshotForUi({
        .as_of_ns = now.time_since_epoch().count(),
        .watch_lists = {{
            .document_id = "watch-list.default",
            .rows = {{
                .row_id = "row.aapl",
                .instrument_id = "asset-aapl",
                .symbol = "AAPL",
            }},
            .needs_change_from_open = true,
        }},
    });

    ASSERT_NE(snapshot.markets.Find("asset-aapl"), nullptr);
    ASSERT_EQ(snapshot.watch_lists.size(), 1U);
    ASSERT_EQ(snapshot.watch_lists.front().rows.size(), 1U);
    EXPECT_TRUE(snapshot.watch_lists.front().rows.front().history_missing);
}

TEST(ChartApplicationSnapshotTest,
     WatchListDailyHistoryRangeIsStableWithinOneUtcDay) {
    TemporaryDatabase temporary;
    Database database;
    std::string error;
    ASSERT_TRUE(database.OpenAt(temporary.path, error)) << error;
    tradebox::application::TradingApplication application(database);
    constexpr std::int64_t kDayNs = 24LL * 60LL * 60LL * 1'000'000'000LL;
    const auto query = [](std::int64_t as_of_ns) {
        return tradebox::application::UiSnapshotQuery{
            .as_of_ns = as_of_ns,
            .watch_lists = {{
                .document_id = "watch-list.default",
                .rows = {{.row_id = "row.aapl",
                          .instrument_id = "asset-aapl",
                          .symbol = "AAPL"}},
                .needs_change_from_open = true,
            }},
        };
    };
    const auto first = application.SnapshotForUi(query(100 * kDayNs + 1));
    const auto second = application.SnapshotForUi(
        query(100 * kDayNs + 12 * 60LL * 60LL * 1'000'000'000LL));

    ASSERT_EQ(first.watch_lists.size(), 1U);
    ASSERT_EQ(second.watch_lists.size(), 1U);
    EXPECT_EQ(first.watch_lists.front().daily_range,
              second.watch_lists.front().daily_range);
    EXPECT_EQ(first.watch_lists.front().daily_range.start_ns, 55 * kDayNs);
    EXPECT_EQ(first.watch_lists.front().daily_range.end_ns, 101 * kDayNs);
}

}  // namespace
