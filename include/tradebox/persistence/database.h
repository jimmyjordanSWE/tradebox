#pragma once

#include "tradebox/core/order_command.h"
#include "tradebox/core/asset_catalog.h"
#include "tradebox/core/bar_series.h"
#include "tradebox/core/market_data.h"
#include "tradebox/ui/model.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct sqlite3;

struct WindowPlacement {
    int x = 0;
    int y = 0;
    int width = 1440;
    int height = 900;
    bool maximized = false;
    bool exists = false;
};

struct MarketDataStorageUsage {
    std::uint64_t candlestick_bytes = 0;
    std::uint64_t tick_bytes = 0;
    std::uint64_t database_bytes = 0;
    std::uint64_t candlestick_rows = 0;
    std::uint64_t tick_rows = 0;
};

struct DatabaseWriterTelemetry {
    std::uint64_t pending_events = 0;
    std::uint64_t high_water_events = 0;
    std::uint64_t accepted_events = 0;
    std::uint64_t dequeued_events = 0;
    std::uint64_t dropped_market_events = 0;
    std::uint64_t dropped_timeline_events = 0;
    std::uint64_t pending_bar_batches = 0;
    std::uint64_t pending_bars = 0;
    std::uint64_t high_water_bars = 0;
    std::uint64_t accepted_bars = 0;
    std::uint64_t dequeued_bars = 0;
    std::uint64_t dropped_bars = 0;
};

struct StoredMarketTick {
    std::string feed;
    std::string source_event_id;
    std::string kind;
    std::string symbol;
    std::int64_t event_time_ns = 0;
    std::int64_t received_at_ms = 0;
    std::string raw_payload;
};

struct StoredBarSeries {
    tradebox::core::BarSeriesKey key;
    std::string symbol;
    std::vector<tradebox::core::MarketBar> bars;
    std::vector<tradebox::core::BarRange> coverage;
};

class Database {
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool Open(std::string& error);
    bool OpenAt(const std::filesystem::path& database_path,
                std::string& error);
    std::vector<std::string> LoadWatchlist();
    void SaveWatchlist(const std::vector<std::string>& symbols);
    std::vector<tradebox::core::TradableAsset> LoadAssetCatalog();
    void SaveAssetCatalog(const std::vector<tradebox::core::TradableAsset>& assets);
    WindowPlacement LoadWindowPlacement();
    void SaveWindowPlacement(const WindowPlacement& placement);
    std::optional<std::string> LoadAppSetting(std::string_view key);
    void SaveAppSetting(std::string_view key, std::string_view value);
    std::optional<bool> LoadLastConnectedPaper();
    void SaveLastConnectedPaper(bool paper);
    void ClearLastConnectedAccount(bool paper);
    std::string LoadAccountAlias(const std::string& account_id);
    void SaveAccountAlias(const std::string& account_id,
                          const std::string& account_number,
                          const std::string& alias);
    std::vector<Bar> LoadBars(const std::string& symbol);
    void StoreBars(const std::string& symbol, const std::vector<Bar>& bars);
    void StoreProviderBars(
        const tradebox::core::BarUpsertBatch& batch);
    bool QueueProviderBars(
        tradebox::core::BarUpsertBatch batch);
    StoredBarSeries LoadProviderBars(
        const tradebox::core::BarSeriesKey& key,
        tradebox::core::BarRange range);
    void QueueTimelineEvent(std::string source, std::string source_event_id,
                            std::string kind, std::string symbol,
                            std::int64_t event_time_ms, std::string payload);
    void QueueMarketTickEvent(std::string feed,
                              std::string source_event_id,
                              std::string kind, std::string symbol,
                              std::int64_t event_time_ns,
                              std::int64_t received_at_ms,
                              std::string payload);
    void QueueMarketDataEvent(
        std::string feed, std::string source_event_id,
        tradebox::core::MarketDataEventPtr event);
    void FlushQueuedWrites();
    void StoreMarketTickEvents(
        const std::vector<StoredMarketTick>& events);
    tradebox::core::TickSeries LoadMarketTicks(
        const tradebox::core::TickQuery& query);
    std::vector<tradebox::core::TickCoverage>
    MissingMarketTickCoverage(
        const tradebox::core::TickQuery& query,
        std::string_view kind);
    void MarkMarketTickCoverage(
        const tradebox::core::TickQuery& query,
        std::string_view kind,
        tradebox::core::TickCoverage coverage);
    MarketDataStorageUsage LoadMarketDataStorageUsage();
    DatabaseWriterTelemetry WriterTelemetry() const;
    bool AppendCoreEvent(std::string source_event_id, std::string kind,
                         std::uint64_t generation,
                         std::int64_t received_at_ms, std::string payload,
                         std::string& error);
    std::expected<tradebox::core::ReservationResult, std::string>
    ReserveOrderCommand(
        const tradebox::core::OrderCommandRecord& record,
        const tradebox::core::NativeOrderCommand& command);
    std::expected<void, std::string> CompleteOrderCommand(
        const tradebox::core::OrderCommandResult& result);
    std::expected<tradebox::core::OrderCommandLookup, std::string>
    LookupOrderCommand(const std::string& request_id);

    const std::filesystem::path& DataDirectory() const { return data_directory_; }

private:
    struct PendingEvent {
        bool market_tick = false;
        std::string source;
        std::string source_event_id;
        std::string kind;
        std::string symbol;
        std::int64_t event_time = 0;
        std::int64_t available_at_ms = 0;
        std::string payload;
        tradebox::core::MarketDataEventPtr market_data_event;
    };

    void WriterLoop();
    void FlushEvents(std::vector<PendingEvent>& events);
    void FlushProviderBars(
        const std::vector<tradebox::core::BarUpsertBatch>& batches);
    void StoreProviderBarBatchesLocked(
        const std::vector<tradebox::core::BarUpsertBatch>& batches);
    bool Execute(const char* sql, std::string* error = nullptr);
    bool ExecuteMarket(const char* sql, std::string* error = nullptr);

    sqlite3* db_ = nullptr;
    sqlite3* market_db_ = nullptr;
    std::filesystem::path data_directory_;
    std::mutex db_mutex_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::condition_variable flush_cv_;
    std::deque<PendingEvent> pending_;
    std::deque<tradebox::core::BarUpsertBatch> pending_bar_batches_;
    std::size_t pending_bar_count_ = 0;
    bool stopping_ = false;
    std::uint64_t flush_requested_ = 0;
    std::uint64_t flush_completed_ = 0;
    std::uint64_t queue_high_water_ = 0;
    std::uint64_t accepted_events_ = 0;
    std::uint64_t dequeued_events_ = 0;
    std::uint64_t dropped_market_events_ = 0;
    std::uint64_t dropped_timeline_events_ = 0;
    std::uint64_t bar_high_water_ = 0;
    std::uint64_t accepted_bars_ = 0;
    std::uint64_t dequeued_bars_ = 0;
    std::uint64_t dropped_bars_ = 0;
    std::thread writer_;
};
