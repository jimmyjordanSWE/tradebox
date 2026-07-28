#include "tradebox/broker/alpaca_market_stream_decoder.h"
#include "tradebox/core/market_data_store.h"
#include "tradebox/persistence/database.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        const auto unique =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        directory = std::filesystem::temp_directory_path() /
                    ("tradebox-market-replay-" +
                     std::to_string(unique));
        path = directory / "application.db";
    }

    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    std::filesystem::path directory;
    std::filesystem::path path;
};

double EventsPerSecond(
    std::uint64_t events,
    std::chrono::steady_clock::duration elapsed) {
    const double seconds =
        std::chrono::duration<double>(elapsed).count();
    return seconds > 0
               ? static_cast<double>(events) / seconds
               : 0;
}

int Fail(const std::string& message) {
    std::cerr << "REPLAY BENCHMARK FAILED: " << message << '\n';
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path corpus =
        argc > 1 ? argv[1]
                 : std::filesystem::path(
                       TRADEBOX_REPLAY_CORPUS);
    const std::uint64_t expected_events =
        argc > 2 ? std::stoull(argv[2])
                 : TRADEBOX_REPLAY_EVENT_COUNT;
    const std::filesystem::path result_path =
        argc > 3 ? argv[3] : std::filesystem::path{};
    std::ifstream input(corpus, std::ios::binary);
    if (!input) return Fail("could not open " + corpus.string());

    TemporaryDatabase temporary;
    tradebox::core::MarketDataStore market_data(2'000);
    std::uint64_t decoded_events = 0;
    std::uint64_t control_events = 0;
    DatabaseWriterTelemetry writer;
    const auto total_start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point ingestion_start;
    std::chrono::steady_clock::time_point ingestion_end;
    {
        Database database;
        std::string error;
        if (!database.OpenAt(temporary.path, error))
            return Fail("database open failed: " + error);
        ingestion_start = std::chrono::steady_clock::now();

        std::string frame_text;
        while (std::getline(input, frame_text)) {
            if (frame_text.empty()) continue;
            auto frame =
                tradebox::broker::alpaca::DecodeMarketFrame(
                    frame_text, 1'783'511'400'000,
                    [](std::string_view symbol) {
                        return "synthetic:" +
                               std::string(symbol);
                    });
            control_events += frame.controls.size();
            for (auto& item : frame.items) {
                ++decoded_events;
                if (item.market_tick) {
                    database.QueueMarketDataEvent(
                        "iex", std::move(item.source_event_id),
                        item.market_event);
                }
                if (item.market_event)
                    market_data.Ingest(
                        std::move(item.market_event));
            }
        }
        ingestion_end = std::chrono::steady_clock::now();
        writer = database.WriterTelemetry();
        if (writer.dropped_market_events != 0 ||
            writer.dropped_timeline_events != 0)
            return Fail("persistence queue dropped events");
    }
    const auto total_end = std::chrono::steady_clock::now();

    if (decoded_events != expected_events)
        return Fail(
            "decoded " + std::to_string(decoded_events) +
            " events; expected " +
            std::to_string(expected_events));
    if (control_events != 2)
        return Fail("expected authentication and subscription controls");
    for (int index = 1; index <= 10; ++index) {
        std::ostringstream symbol;
        symbol << "TST" << std::setw(3) << std::setfill('0')
               << index;
        const auto snapshot = market_data.Snapshot(symbol.str());
        if (snapshot.instrument_id.empty())
            return Fail("missing state for " + symbol.str());
    }

    std::uint64_t persisted_rows = 0;
    {
        Database database;
        std::string error;
        if (!database.OpenAt(temporary.path, error))
            return Fail("database reopen failed: " + error);
        persisted_rows =
            database.LoadMarketDataStorageUsage().tick_rows;
    }
    if (persisted_rows != decoded_events)
        return Fail(
            "persisted " + std::to_string(persisted_rows) +
            " rows for " + std::to_string(decoded_events) +
            " decoded events");

    const double ingestion_rate = EventsPerSecond(
        decoded_events, ingestion_end - ingestion_start);
    const double end_to_end_rate = EventsPerSecond(
        decoded_events, total_end - total_start);
    std::cout << std::fixed << std::setprecision(0)
              << "MARKET REPLAY | events " << decoded_events
              << " | decode+store+enqueue " << ingestion_rate
              << " events/s | including durable flush "
              << end_to_end_rate
              << " events/s | queue peak "
              << writer.high_water_events << '\n';
    if (!result_path.empty()) {
        std::error_code error;
        std::filesystem::create_directories(
            result_path.parent_path(), error);
        if (error)
            return Fail(
                "could not create benchmark result directory: " +
                error.message());
        std::ofstream result_file(
            result_path, std::ios::binary | std::ios::trunc);
        if (!result_file)
            return Fail(
                "could not write benchmark result " +
                result_path.string());
        result_file << std::fixed << std::setprecision(0)
                    << "{\n"
                    << "  \"events\": " << decoded_events << ",\n"
                    << "  \"decode_store_enqueue_events_per_second\": "
                    << ingestion_rate << ",\n"
                    << "  \"durable_events_per_second\": "
                    << end_to_end_rate << ",\n"
                    << "  \"queue_high_water_events\": "
                    << writer.high_water_events << ",\n"
                    << "  \"dropped_events\": 0,\n"
                    << "  \"persisted_rows\": "
                    << persisted_rows << "\n"
                    << "}\n";
    }

#ifdef NDEBUG
    constexpr double kMinimumIngestionEventsPerSecond = 20'000;
    constexpr double kMinimumEndToEndEventsPerSecond = 5'000;
    if (ingestion_rate < kMinimumIngestionEventsPerSecond)
        return Fail("hot-path throughput fell below 20,000 events/s");
    if (end_to_end_rate < kMinimumEndToEndEventsPerSecond)
        return Fail("durable throughput fell below 5,000 events/s");
#endif
    return 0;
}
