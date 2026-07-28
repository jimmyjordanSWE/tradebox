#pragma once

#include "tradebox/core/order_command.h"
#include "tradebox/ui/model.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
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
    WindowPlacement LoadWindowPlacement();
    void SaveWindowPlacement(const WindowPlacement& placement);
    std::optional<bool> LoadLastConnectedPaper();
    void SaveLastConnectedPaper(bool paper);
    void ClearLastConnectedAccount(bool paper);
    std::string LoadAccountAlias(const std::string& account_id);
    void SaveAccountAlias(const std::string& account_id,
                          const std::string& account_number,
                          const std::string& alias);
    std::vector<Bar> LoadBars(const std::string& symbol);
    void StoreBars(const std::string& symbol, const std::vector<Bar>& bars);
    void QueueTimelineEvent(std::string source, std::string source_event_id,
                            std::string kind, std::string symbol,
                            std::int64_t event_time_ms, std::string payload);
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
        std::string source;
        std::string source_event_id;
        std::string kind;
        std::string symbol;
        std::int64_t event_time_ms = 0;
        std::int64_t available_at_ms = 0;
        std::string payload;
    };

    void WriterLoop();
    void FlushEvents(std::vector<PendingEvent>& events);
    bool Execute(const char* sql, std::string* error = nullptr);

    sqlite3* db_ = nullptr;
    std::filesystem::path data_directory_;
    std::mutex db_mutex_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<PendingEvent> pending_;
    bool stopping_ = false;
    std::thread writer_;
};
