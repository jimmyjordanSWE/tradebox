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
#include <string_view>
#include <variant>
#include <vector>

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
    const std::string feed_name =
        argc > 4 ? argv[4] : "iex";
    const std::size_t expected_instruments =
        argc > 5 ? std::stoull(argv[5]) : 10;
    bool profile_phases = false;
    auto json_backend =
        tradebox::broker::alpaca::
            MarketJsonBackend::DirectWithRapidFallback;
    std::string_view json_backend_name =
        "direct-rapidjson";
    for (int argument = 6; argument < argc; ++argument) {
        const std::string_view option(argv[argument]);
        if (option == "profile") {
            profile_phases = true;
        } else if (option == "direct-rapidjson") {
            json_backend =
                tradebox::broker::alpaca::
                    MarketJsonBackend::
                        DirectWithRapidFallback;
            json_backend_name = option;
        } else if (option == "rapidjson") {
            json_backend =
                tradebox::broker::alpaca::
                    MarketJsonBackend::RapidJsonSax;
            json_backend_name = option;
        } else {
            return Fail(
                "unknown benchmark option: " +
                std::string(option));
        }
    }
    const bool sip_soak = feed_name == "sip";
    if (feed_name != "iex" && !sip_soak)
        return Fail("feed must be iex or sip");
    if (expected_instruments == 0)
        return Fail("expected instrument count must be positive");

    std::vector<std::string> symbols;
    symbols.reserve(expected_instruments);
    for (std::size_t index = 1;
         index <= expected_instruments; ++index) {
        std::ostringstream symbol;
        symbol << "TST" << std::setw(3) << std::setfill('0')
               << index;
        symbols.push_back(symbol.str());
    }
    std::ifstream input(corpus, std::ios::binary);
    if (!input) return Fail("could not open " + corpus.string());
    // Corpus I/O and the Windows file-cache state are not part of the
    // websocket decode/store/enqueue path being measured. Load frames before
    // starting either throughput clock so repeated runs measure the same work.
    std::vector<std::string> frames;
    std::string frame_text;
    while (std::getline(input, frame_text))
        if (!frame_text.empty())
            frames.push_back(std::move(frame_text));
    if (frames.empty()) return Fail("market replay corpus is empty");

    TemporaryDatabase temporary;
    tradebox::core::MarketDataStore market_data(2'000);
    std::uint64_t decoded_events = 0;
    std::uint64_t control_events = 0;
    std::uint64_t correction_events = 0;
    std::uint64_t cancellation_events = 0;
    std::chrono::steady_clock::duration decode_elapsed{};
    std::chrono::steady_clock::duration persistence_enqueue_elapsed{};
    std::chrono::steady_clock::duration projection_elapsed{};
    std::chrono::steady_clock::duration flush_elapsed{};
    DatabaseWriterTelemetry writer;
    bool reconnected = false;
    bool persistence_rejected = false;
    const auto total_start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point ingestion_start;
    std::chrono::steady_clock::time_point ingestion_end;
    {
        Database database;
        std::string error;
        if (!database.OpenAt(temporary.path, error))
            return Fail("database open failed: " + error);
        market_data.Ingest(
            tradebox::core::MarketStreamChanged{
                .status =
                    tradebox::core::MarketStreamStatus::Subscribed,
                .feed = sip_soak
                            ? tradebox::core::MarketDataFeed::Sip
                            : tradebox::core::MarketDataFeed::Iex,
                .trade_symbols = symbols,
                .quote_symbols = symbols,
                .status_symbols = {"*"},
                .message = "Synthetic stream subscribed",
                .received_at_ms = 1'783'511'400'000,
            });
        ingestion_start = std::chrono::steady_clock::now();

        bool stale_observed = false;
        const std::uint64_t stale_at =
            expected_events / 3;
        for (const std::string& encoded_frame : frames) {
            const auto decode_start =
                profile_phases
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
            auto frame =
                tradebox::broker::alpaca::DecodeMarketFrame(
                    encoded_frame, 1'783'511'400'000,
                    [](std::string_view symbol) {
                        return "synthetic:" +
                               std::string(symbol);
                    },
                    json_backend);
            if (profile_phases)
                decode_elapsed +=
                    std::chrono::steady_clock::now() -
                    decode_start;
            control_events += frame.controls.size();
            std::vector<QueuedMarketDataEvent>
                persistence_events;
            std::vector<tradebox::core::MarketDataEventPtr>
                projection_events;
            persistence_events.reserve(frame.items.size());
            projection_events.reserve(frame.items.size());
            for (auto& item : frame.items) {
                ++decoded_events;
                if (item.market_event &&
                    std::holds_alternative<
                        tradebox::core::TradeCorrected>(
                        *item.market_event))
                    ++correction_events;
                if (item.market_event &&
                    std::holds_alternative<
                        tradebox::core::TradeCanceled>(
                        *item.market_event))
                    ++cancellation_events;
                if (item.market_tick) {
                    persistence_events.push_back({
                        .source_event_id =
                            std::move(item.source_event_id),
                        .event = item.market_event,
                    });
                }
                if (item.market_event)
                    projection_events.push_back(
                        std::move(item.market_event));
            }
            const auto enqueue_start =
                profile_phases
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::
                          time_point{};
            if (!database.QueueMarketDataEvents(
                    feed_name,
                    std::move(persistence_events)))
                persistence_rejected = true;
            if (profile_phases)
                persistence_enqueue_elapsed +=
                    std::chrono::steady_clock::now() -
                    enqueue_start;
            const auto projection_start =
                profile_phases
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::
                          time_point{};
            market_data.IngestBatch(
                std::move(projection_events));
            if (profile_phases)
                projection_elapsed +=
                    std::chrono::steady_clock::now() -
                    projection_start;
            if (sip_soak && !stale_observed &&
                decoded_events >= stale_at) {
                market_data.Ingest(
                    tradebox::core::MarketStreamChanged{
                        .status =
                            tradebox::core::
                                MarketStreamStatus::Stale,
                        .feed =
                            tradebox::core::
                                MarketDataFeed::Sip,
                        .message =
                            "Synthetic connection gap",
                        .received_at_ms =
                            1'783'511'400'100,
                    });
                stale_observed = true;
                if (market_data.Snapshot(symbols.front())
                        .stream_status !=
                    tradebox::core::MarketStreamStatus::Stale)
                    return Fail(
                        "synthetic disconnect did not "
                        "publish stale state");
                market_data.Ingest(
                    tradebox::core::MarketStreamChanged{
                        .status =
                            tradebox::core::
                                MarketStreamStatus::Subscribed,
                        .feed =
                            tradebox::core::
                                MarketDataFeed::Sip,
                        .trade_symbols = symbols,
                        .quote_symbols = symbols,
                        .status_symbols = {"*"},
                        .message =
                            "Synthetic stream recovered",
                        .received_at_ms =
                            1'783'511'400'200,
                    });
                reconnected = true;
            }
        }
        ingestion_end = std::chrono::steady_clock::now();
        const auto flush_start =
            profile_phases
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
        const auto flushed = database.FlushQueuedWrites();
        if (profile_phases)
            flush_elapsed =
                std::chrono::steady_clock::now() - flush_start;
        if (!flushed)
            return Fail(
                "persistence flush failed: " + flushed.error());
        writer = database.WriterTelemetry();
        if (persistence_rejected ||
            writer.dropped_market_events != 0 ||
            writer.dropped_timeline_events != 0)
            return Fail("persistence queue dropped events");
        if (writer.write_failures != 0)
            return Fail(
                "persistence writer failed: " +
                writer.last_write_error);
    }
    const auto total_end = std::chrono::steady_clock::now();

    if (decoded_events != expected_events)
        return Fail(
            "decoded " + std::to_string(decoded_events) +
            " events; expected " +
            std::to_string(expected_events));
    if (control_events != 2)
        return Fail("expected authentication and subscription controls");
    if (sip_soak && !reconnected)
        return Fail(
            "SIP soak did not exercise stale/reconnect recovery");
    if (correction_events < 1'000 ||
        cancellation_events < 1'000)
        return Fail(
            "correction-heavy corpus has insufficient "
            "correction or cancellation events");
    for (const std::string& symbol : symbols) {
        const auto snapshot = market_data.Snapshot(symbol);
        if (snapshot.instrument_id.empty())
            return Fail("missing state for " + symbol);
        if (snapshot.stream_status !=
            tradebox::core::MarketStreamStatus::Subscribed)
            return Fail(
                "stream did not recover for " + symbol);
        if (sip_soak &&
            snapshot.feed !=
                tradebox::core::MarketDataFeed::Sip)
            return Fail("wrong feed after SIP recovery for " +
                        symbol);
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
    const double target_ingestion_events_per_second =
        sip_soak ? 90'000 : 120'000;
    constexpr double kTargetEndToEndEventsPerSecond = 90'000;
    const bool release_target_met =
        ingestion_rate >= target_ingestion_events_per_second &&
        end_to_end_rate >= kTargetEndToEndEventsPerSecond;
    std::cout << std::fixed << std::setprecision(0)
              << "MARKET REPLAY | events " << decoded_events
              << " | decode+store+enqueue " << ingestion_rate
              << " events/s | including durable flush "
              << end_to_end_rate
              << " events/s | corrections "
              << correction_events
              << " | cancellations "
              << cancellation_events
              << " | queue peak "
              << writer.high_water_events
              << " | feed " << feed_name
              << " | json " << json_backend_name
              << " | instruments " << expected_instruments
              << '\n';
    if (profile_phases) {
        const auto milliseconds = [](const auto elapsed) {
            return std::chrono::duration<double, std::milli>(
                       elapsed)
                .count();
        };
        std::cout << std::fixed << std::setprecision(2)
                  << "PHASE PROFILE | decode "
                  << milliseconds(decode_elapsed)
                  << " ms | persistence enqueue "
                  << milliseconds(persistence_enqueue_elapsed)
                  << " ms | in-memory projection "
                  << milliseconds(projection_elapsed)
                  << " ms | durable flush "
                  << milliseconds(flush_elapsed) << " ms\n";
    }
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
                    << "  \"release_target_met\": "
                    << (release_target_met ? "true" : "false")
                    << ",\n"
                    << "  \"queue_high_water_events\": "
                    << writer.high_water_events << ",\n"
                    << "  \"correction_events\": "
                    << correction_events << ",\n"
                    << "  \"cancellation_events\": "
                    << cancellation_events << ",\n"
                    << "  \"dropped_events\": 0,\n"
                    << "  \"persisted_rows\": "
                    << persisted_rows << ",\n"
                    << "  \"feed\": \"" << feed_name
                    << "\",\n"
                    << "  \"instruments\": "
                    << expected_instruments << ",\n"
                    << "  \"stale_reconnect_recovered\": "
                    << (sip_soak ? "true" : "false")
                    << "\n"
                    << "}\n";
    }

#ifdef NDEBUG
    // Absolute workstation throughput varies with power policy, antivirus,
    // and concurrent load. Keep a hard regression floor in default CTest,
    // while reporting the release target separately for packaging runs.
    constexpr double kRegressionFloorEventsPerSecond = 60'000;
    if (ingestion_rate < kRegressionFloorEventsPerSecond)
        return Fail(
            "ingestion throughput fell below the 60,000 events/s "
            "regression floor");
    if (end_to_end_rate < kRegressionFloorEventsPerSecond)
        return Fail(
            "durable throughput fell below the 60,000 events/s "
            "regression floor");
    if (!release_target_met)
        std::cerr
            << "REPLAY TARGET NOT MET: release target is "
            << target_ingestion_events_per_second
            << " ingestion and "
            << kTargetEndToEndEventsPerSecond
            << " durable events/s; run on the packaging performance "
               "profile before release.\n";
#endif
    return 0;
}
