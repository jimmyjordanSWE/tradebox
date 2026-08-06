#include "tradebox/application/trading_application.h"

#include "tradebox/application/order_execution_service.h"
#include "tradebox/application/history_request_tracker.h"
#include "tradebox/broker/alpaca_service.h"
#include "tradebox/core/system_clock.h"
#include "tradebox/core/bar_store.h"
#include "tradebox/core/market_data_store.h"
#include "tradebox/core/trading_core.h"
#include "tradebox/persistence/database_event_journal.h"
#include "tradebox/persistence/database_order_command_journal.h"
#include "tradebox/platform/credentials.h"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace tradebox::application {
namespace {

void ClearSensitiveString(std::string& value) noexcept {
    volatile char* bytes = value.data();
    for (std::size_t index = 0; index < value.size(); ++index)
        bytes[index] = '\0';
    value.clear();
}

std::int64_t WallClockNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

constexpr std::int64_t kAssetCatalogRefreshIntervalMs =
    24LL * 60LL * 60LL * 1000LL;

core::BarRange WatchListDailyRange(std::int64_t as_of_ns) {
    constexpr std::int64_t kDayNs = 24LL * 60LL * 60LL * 1'000'000'000LL;
    constexpr std::int64_t kLookbackDays = 45;
    return {
        as_of_ns - kLookbackDays * kDayNs,
        as_of_ns + kDayNs,
    };
}

class CoreValuationMarketDataSink final
    : public core::IMarketDataSink {
public:
    CoreValuationMarketDataSink(
        core::MarketDataStore& store,
        core::TradingCore& trading_core)
        : store_(store), trading_core_(trading_core) {}

    void Ingest(core::MarketDataEventPtr event) override {
        if (!event) return;
        store_.Ingest(event);
        std::visit(
            [this](const auto& typed) {
                using T = std::decay_t<decltype(typed)>;
                if constexpr (
                    std::is_same_v<T,
                                   core::MarketStreamChanged>) {
                    const core::CoreSnapshot account =
                        trading_core_.Snapshot();
                    for (const core::PositionState& position :
                         account.positions) {
                        const std::string& identifier =
                            position.asset_id.empty()
                                ? position.symbol
                                : position.asset_id;
                        trading_core_.ApplyMarketData(
                            store_.Snapshot(identifier));
                    }
                } else if constexpr (
                    std::is_same_v<T, core::QuoteReceived>) {
                    Apply(typed.quote.instrument_id,
                          typed.quote.symbol);
                } else if constexpr (
                    std::is_same_v<T, core::TradeReceived>) {
                    Apply(typed.trade.instrument_id,
                          typed.trade.symbol);
                } else if constexpr (
                    std::is_same_v<T, core::TradeCorrected>) {
                    Apply(typed.instrument_id, typed.symbol);
                } else if constexpr (
                    std::is_same_v<
                        T, core::TradingStatusReceived>) {
                    Apply(typed.status.instrument_id,
                          typed.status.symbol);
                } else {
                    Apply(typed.instrument_id, typed.symbol);
                }
            },
            *event);
    }

private:
    void Apply(const std::string& instrument_id,
               const std::string& symbol) {
        trading_core_.ApplyMarketData(
            store_.Snapshot(
                instrument_id.empty() ? symbol
                                      : instrument_id));
    }

    core::MarketDataStore& store_;
    core::TradingCore& trading_core_;
};

}  // namespace

class TradingApplication::Impl final {
public:
    explicit Impl(Database& database)
        : database(database),
          owned_events(std::make_unique<UiEventQueue>()),
          event_queue(owned_events.get()),
          journal(database),
          order_journal(database),
          core(journal, clock),
          valuation_market_data(market_data, core),
          broker(*owned_events, database, core,
                 valuation_market_data,
                 market_data, bars, &history_requests),
          order_execution(core, broker, order_journal, clock,
                          &market_data) {
        ReloadAssetCatalogCache();
    }

    Impl(Database& database,
         core::IOrderCommandJournal& external_order_journal)
        : database(database),
          owned_events(std::make_unique<UiEventQueue>()),
          event_queue(owned_events.get()),
          journal(database),
          order_journal(database),
          core(journal, clock),
          valuation_market_data(market_data, core),
          broker(*owned_events, database, core,
                 valuation_market_data,
                 market_data, bars, &history_requests),
          order_execution(core, broker, external_order_journal, clock,
                          &market_data) {
        ReloadAssetCatalogCache();
    }

    Impl(UiEventQueue& events, Database& database)
        : database(database),
          event_queue(&events),
          journal(database),
          order_journal(database),
          core(journal, clock),
          valuation_market_data(market_data, core),
          broker(events, database, core,
                 valuation_market_data,
                 market_data, bars, &history_requests),
          order_execution(core, broker, order_journal, clock,
                          &market_data) {
        ReloadAssetCatalogCache();
    }

    void ReloadAssetCatalogCache() {
        auto catalog = database.LoadAssetCatalog();
        auto stored = database.LoadKnownProviderBarAssets();
        std::unique_lock lock(asset_catalog_mutex);
        asset_catalog = std::move(catalog);
        stored_asset_catalog = std::move(stored);
    }

    [[nodiscard]] bool AssetCatalogIsFresh(std::int64_t now_ms) const {
        std::shared_lock lock(asset_catalog_mutex);
        if (asset_catalog.empty()) return false;
        const auto newest = std::ranges::max_element(
            asset_catalog, {}, &core::TradableAsset::received_at_ms);
        return newest != asset_catalog.end() &&
               newest->received_at_ms > 0 &&
               newest->received_at_ms <= now_ms &&
               now_ms - newest->received_at_ms <
                   kAssetCatalogRefreshIntervalMs;
    }

    Database& database;
    std::unique_ptr<UiEventQueue> owned_events;
    UiEventQueue* event_queue = nullptr;
    mutable std::shared_mutex asset_catalog_mutex;
    std::vector<core::TradableAsset> asset_catalog;
    std::vector<core::TradableAsset> stored_asset_catalog;
    persistence::DatabaseEventJournal journal;
    persistence::DatabaseOrderCommandJournal order_journal;
    core::SystemClock clock;
    core::TradingCore core;
    core::MarketDataStore market_data;
    CoreValuationMarketDataSink valuation_market_data;
    core::BarStore bars;
    HistoryRequestTracker history_requests;
    mutable IndicatorProjectionCache indicator_projections;
    MarketDataInterestCoordinator market_interests;
    core::MarketDataFeed active_market_feed =
        core::MarketDataFeed::Iex;
    std::vector<std::string> active_market_symbols;
    AlpacaService broker;
    OrderExecutionService order_execution;
};

TradingApplication::TradingApplication(Database& database)
    : impl_(std::make_unique<Impl>(database)) {}

TradingApplication::TradingApplication(
    Database& database,
    core::IOrderCommandJournal& order_journal)
    : impl_(std::make_unique<Impl>(database, order_journal)) {}

TradingApplication::TradingApplication(UiEventQueue& events,
                                       Database& database)
    : impl_(std::make_unique<Impl>(events, database)) {}

TradingApplication::~TradingApplication() = default;

core::CoreSnapshot TradingApplication::Snapshot() const {
    return impl_->core.Snapshot();
}

ApplicationUiSnapshot TradingApplication::SnapshotForUi(
    const UiSnapshotQuery& query) const {
    ApplicationUiSnapshot result;
    result.core = Snapshot();
    std::vector<std::string> visible_market_identifiers =
        query.market_symbols;
    visible_market_identifiers.reserve(
        visible_market_identifiers.size() + query.charts.size());
    for (const UiChartQuery& chart : query.charts) {
        const std::string& identifier = chart.key.instrument_id.empty()
                                            ? chart.symbol
                                            : chart.key.instrument_id;
        if (!identifier.empty())
            visible_market_identifiers.push_back(identifier);
    }
    result.markets = impl_->market_data.SnapshotFrame(
        visible_market_identifiers);

    result.charts.reserve(query.charts.size());
    for (const UiChartQuery& chart : query.charts) {
        UiChartSnapshot projected{
            .document_id = chart.document_id,
        };
        if (chart.document_id.empty() ||
            chart.key.instrument_id.empty() || chart.symbol.empty() ||
            chart.key.timeframe.empty() ||
            chart.key.feed == core::MarketDataFeed::Unknown ||
            chart.range.start_ns < 0 ||
            chart.range.start_ns >= chart.range.end_ns) {
            projected.message =
                "Chart query requires a stable document, instrument, and valid range";
            result.charts.push_back(std::move(projected));
            continue;
        }

        const std::string& identifier = chart.key.instrument_id.empty()
                                            ? chart.symbol
                                            : chart.key.instrument_id;
        const core::MarketDataSnapshot* live =
            result.markets.Find(identifier);
        projected.series = live == nullptr
                               ? core::BarSeriesSnapshot{}
                               : BarsFromMarketSnapshot(
                                     chart.key, chart.range, live);
        const auto request = impl_->history_requests.StatusFor(
            chart.key, chart.range);
        if (request && request->state ==
                           broker::HistoryRequestState::Loading) {
            projected.status = ChartDataStatus::Loading;
            projected.message = "Loading chart history";
        } else if (request && request->state ==
                                  broker::HistoryRequestState::Failed) {
            projected.status = ChartDataStatus::Failed;
            projected.message = request->message.empty()
                                    ? "Chart history request failed"
                                    : request->message;
            projected.retryable = true;
        } else if (!projected.series.missing_ranges.empty()) {
            projected.status = result.core.authenticated
                                   ? ChartDataStatus::MissingHistory
                                   : ChartDataStatus::Unavailable;
            projected.message = result.core.authenticated
                                    ? "Chart history is incomplete"
                                    : "History is unavailable while disconnected";
            projected.retryable = result.core.authenticated;
        } else if (projected.series.bars.empty() &&
                   !projected.series.current_bar) {
            projected.status = ChartDataStatus::Empty;
            projected.message = "No bars in this range";
        } else {
            projected.status = ChartDataStatus::Ready;
        }

        projected.indicator_projection =
            impl_->indicator_projections.Resolve(
                projected.series, chart.indicators);
        result.charts.push_back(std::move(projected));
    }
    result.watch_lists.reserve(query.watch_lists.size());
    std::unordered_map<std::string, core::BarSeriesSnapshot> daily_series;
    for (const UiWatchListQuery& watch_list : query.watch_lists) {
        UiWatchListSnapshot projected{
            .document_id = watch_list.document_id,
        };
        if (watch_list.needs_change_from_open && query.as_of_ns > 0)
            projected.daily_range = WatchListDailyRange(query.as_of_ns);
        projected.rows.reserve(watch_list.rows.size());
        for (const UiWatchListRowQuery& row : watch_list.rows) {
            UiWatchListRowSnapshot row_snapshot{.row_id = row.row_id};
            const std::string& identifier = row.instrument_id.empty()
                                                ? row.symbol
                                                : row.instrument_id;
            const core::MarketDataSnapshot* live =
                identifier.empty() ? nullptr : result.markets.Find(identifier);
            if (live != nullptr && live->latest_price)
                row_snapshot.current_price = live->latest_price->price;

            if (watch_list.needs_change_from_open &&
                query.as_of_ns > 0 && !row.instrument_id.empty() &&
                live != nullptr) {
                auto [daily_position, inserted] = daily_series.try_emplace(
                    row.instrument_id);
                auto& daily = daily_position->second;
                if (inserted) {
                    daily = BarsFromMarketSnapshot(
                        {.instrument_id = row.instrument_id,
                         .feed = impl_->active_market_feed,
                         .timeframe = "1Day",
                         .adjustment = core::BarAdjustment::Raw},
                        projected.daily_range, live);
                }
                row_snapshot.history_missing =
                    !daily.missing_ranges.empty();
                if (row_snapshot.current_price && daily.current_bar)
                    row_snapshot.change_from_open =
                        *row_snapshot.current_price - daily.current_bar->open;
            }
            projected.rows.push_back(std::move(row_snapshot));
        }
        result.watch_lists.push_back(std::move(projected));
    }
    if (query.asset_limit != 0) {
        std::shared_lock catalog_lock(impl_->asset_catalog_mutex);
        const auto search = [&](const std::string& text) {
            auto matches = core::SearchTradableAssets(
                impl_->asset_catalog, text, query.asset_limit,
                query.asset_preferred_instrument_ids);
            if (matches.empty() && !text.empty())
                matches = core::SearchTradableAssets(
                    impl_->stored_asset_catalog, text, query.asset_limit,
                    query.asset_preferred_instrument_ids);
            return matches;
        };
        if (!query.asset_search.empty()) {
            result.assets = search(query.asset_search);
            result.asset_search_results.push_back({
                query.asset_search, result.assets});
        }
        for (const std::string& text : query.asset_searches) {
            if (text.empty() || std::ranges::any_of(
                                    result.asset_search_results,
                                    [&](const UiAssetSearchResult& existing) {
                                        return existing.query == text;
                                    }))
                continue;
            result.asset_search_results.push_back({text, search(text)});
        }
    }
    return result;
}

std::optional<core::CoreSnapshot> TradingApplication::SnapshotAfter(
    std::uint64_t revision) const {
    return impl_->core.SnapshotAfter(revision);
}

core::MarketDataSnapshot TradingApplication::MarketData(
    const std::string& symbol) const {
    return impl_->market_data.Snapshot(symbol);
}

core::MarketDataDelta TradingApplication::MarketDataChanges(
    const std::string& symbol, std::uint64_t after_sequence,
    std::size_t maximum_events) const {
    return impl_->market_data.Delta(
        symbol, after_sequence, maximum_events);
}

core::ChangedInstruments TradingApplication::ChangedMarketInstruments(
    std::uint64_t after_sequence,
    std::size_t maximum_instruments) const {
    return impl_->market_data.Changes(
        after_sequence, maximum_instruments);
}

core::BarSeriesSnapshot TradingApplication::Bars(
    const core::BarSeriesKey& key,
    core::BarRange range) const {
    return BarsFromMarketSnapshot(key, range, nullptr);
}

core::BarSeriesSnapshot TradingApplication::BarsFromMarketSnapshot(
    const core::BarSeriesKey& key, core::BarRange range,
    const core::MarketDataSnapshot* published_live) const {
    core::BarSeriesSnapshot snapshot =
        impl_->bars.Bars(key, range);
    if (!snapshot.missing_ranges.empty()) {
        StoredBarSeries stored =
            impl_->database.LoadProviderBars(key, range);
        std::optional<core::BarRange> first_coverage;
        if (!stored.coverage.empty()) {
            first_coverage = stored.coverage.front();
            stored.coverage.erase(stored.coverage.begin());
        }
        impl_->bars.Upsert({
            .key = key,
            .symbol = std::move(stored.symbol),
            .bars = std::move(stored.bars),
            .covered_range = first_coverage,
        });
        for (const core::BarRange coverage : stored.coverage)
            impl_->bars.Upsert({
                .key = key,
                .covered_range = coverage,
            });
        snapshot = impl_->bars.Bars(key, range);
    }

    core::MarketDataSnapshot owned_live;
    if (published_live == nullptr) {
        const std::string identifier = key.instrument_id.empty()
                                           ? snapshot.symbol
                                           : key.instrument_id;
        owned_live = impl_->market_data.Snapshot(identifier);
        published_live = &owned_live;
    }
    const core::MarketDataSnapshot& live = *published_live;

    std::vector<core::MarketBar> base_minutes;
    const auto duration =
        core::FixedBarDurationNs(key.timeframe);
    if (duration && *duration > 60LL * 1'000'000'000 &&
        key.adjustment == core::BarAdjustment::Raw &&
        !live.provisional_minute_bars.empty()) {
        const auto newest = std::ranges::max_element(
            live.provisional_minute_bars, {},
            &core::ProvisionalMinuteBar::start_ns);
        const std::int64_t interval_start =
            (newest->start_ns / *duration) * *duration;
        const core::BarRange base_range{
            interval_start,
            newest->start_ns + 60LL * 1'000'000'000,
        };
        core::BarSeriesKey base_key = key;
        base_key.timeframe = "1Min";
        const StoredBarSeries stored =
            impl_->database.LoadProviderBars(
                base_key, base_range);
        base_minutes = stored.bars;
        const auto in_memory =
            impl_->bars.Bars(base_key, base_range);
        for (const core::MarketBar& minute :
             in_memory.bars) {
            const auto position = std::ranges::lower_bound(
                base_minutes, minute.start_ns, {},
                &core::MarketBar::start_ns);
            if (position != base_minutes.end() &&
                position->start_ns == minute.start_ns)
                *position = minute;
            else
                base_minutes.insert(position, minute);
        }
    }
    core::ConvergeLiveBar(snapshot, live, base_minutes);
    const std::int64_t current_ns =
        snapshot.latest_price
            ? snapshot.latest_price->event_time_ns
            : snapshot.current_bar
                  ? snapshot.current_bar->start_ns
                  : 0;
    if (current_ns > 0) {
        constexpr std::int64_t day_ns =
            24LL * 60 * 60 * 1'000'000'000;
        const std::int64_t current_day = current_ns / day_ns;
        for (auto bar = snapshot.bars.rbegin();
             bar != snapshot.bars.rend(); ++bar) {
            if (bar->start_ns / day_ns < current_day) {
                snapshot.previous_session_close = bar->close;
                break;
            }
        }
    }
    return snapshot;
}

core::BarSeriesSnapshot TradingApplication::BarsForSymbol(
    const std::string& symbol, const std::string& timeframe,
    core::BarRange range) const {
    return Bars(impl_->broker.ResolveBarSeriesKey(symbol, timeframe), range);
}

core::ChangedBarSeriesBatch TradingApplication::ChangedBarSeries(
    std::uint64_t after_sequence,
    std::size_t maximum_series) const {
    return impl_->bars.BarChanges(
        after_sequence, maximum_series);
}

std::expected<core::CommandReceipt, core::CoreError>
TradingApplication::Connect(ConnectionRequest request) {
    const bool missing_key = request.api_key.empty();
    const bool missing_secret = request.api_secret.empty();
    if (missing_key != missing_secret) {
        return std::unexpected(core::CoreError{
            .code = core::CoreErrorCode::InvalidCommand,
            .message = "Both broker credential fields are required",
        });
    }
    if (missing_key) {
        AlpacaCredentials saved;
        std::string credential_error;
        const bool paper =
            request.environment == core::AccountEnvironment::Paper;
        if (!CredentialStore::Load(request.credential_slot, paper, saved,
                                   credential_error)) {
            return std::unexpected(core::CoreError{
                .code = core::CoreErrorCode::InvalidCommand,
                .message = credential_error,
            });
        }
        request.api_key = std::move(saved.key);
        request.api_secret = std::move(saved.secret);
    }
    auto receipt = impl_->core.Submit(
        core::ConnectAccount{request.environment});
    if (!receipt || !receipt->accepted) return receipt;

    AlpacaCredentials credentials(
        request.api_key, request.api_secret,
        request.environment == core::AccountEnvironment::Paper);
    ClearSensitiveString(request.api_key);
    ClearSensitiveString(request.api_secret);
    impl_->active_market_feed = request.market_data_feed;
    const auto bootstrap = impl_->market_interests.Upsert({
        .consumer_id = "connection.bootstrap",
        .feed = request.market_data_feed,
        .symbols = request.market_symbols,
        .priority = MarketDataInterestPriority::UserVisible,
    });
    impl_->active_market_symbols =
        bootstrap ? bootstrap->Symbols() : std::vector<std::string>{};
    impl_->broker.Connect(
        std::move(credentials),
        impl_->active_market_symbols,
        request.market_data_feed);
    if (!impl_->AssetCatalogIsFresh(WallClockNowMs()))
        impl_->broker.RequestAssetCatalog();
    return receipt;
}

bool TradingApplication::HasSavedCredentials(
    std::string_view credential_slot,
    core::AccountEnvironment environment) const {
    return CredentialStore::Exists(
        credential_slot, environment == core::AccountEnvironment::Paper);
}

std::expected<std::vector<SavedAccountDescriptor>, std::string>
TradingApplication::SavedAccounts() const {
    const auto listed = CredentialStore::List();
    if (!listed) return std::unexpected(listed.error());

    std::vector<SavedAccountDescriptor> result;
    result.reserve(listed->size());
    for (const CredentialStore::Descriptor& descriptor : *listed) {
        result.push_back({
            .credential_slot = descriptor.slot,
            .environment = descriptor.paper
                ? core::AccountEnvironment::Paper
                : core::AccountEnvironment::Live,
            .api_key_id = descriptor.api_key_id,
        });
    }
    return result;
}

std::expected<void, std::string> TradingApplication::SaveCredentials(
    std::string_view credential_slot, core::AccountEnvironment environment,
    std::string api_key, std::string api_secret) {
    if (api_key.empty() || api_secret.empty())
        return std::unexpected("Both broker credential fields are required");
    if (credential_slot.empty())
        return std::unexpected("An account name is required");
    AlpacaCredentials credentials(
        std::move(api_key), std::move(api_secret),
        environment == core::AccountEnvironment::Paper);
    std::string error;
    if (!CredentialStore::Save(credential_slot, credentials, error))
        return std::unexpected(std::move(error));
    return {};
}

std::expected<void, std::string> TradingApplication::RenameAccount(
    std::string_view old_credential_slot,
    std::string_view new_credential_slot,
    core::AccountEnvironment environment) {
    if (old_credential_slot.empty() || new_credential_slot.empty())
        return std::unexpected("An account name is required");
    std::string error;
    if (!CredentialStore::Rename(
            old_credential_slot, new_credential_slot,
            environment == core::AccountEnvironment::Paper, error))
        return std::unexpected(std::move(error));
    return {};
}

std::expected<void, std::string> TradingApplication::ForgetCredentials(
    std::string_view credential_slot, core::AccountEnvironment environment) {
    std::string error;
    if (!CredentialStore::Delete(
            credential_slot, environment == core::AccountEnvironment::Paper,
            error))
        return std::unexpected(std::move(error));
    return {};
}

std::expected<core::CommandReceipt, core::CoreError>
TradingApplication::Disconnect() {
    impl_->broker.Disconnect();
    return impl_->core.Submit(core::DisconnectAccount{});
}

std::future<core::OrderCommandResult> TradingApplication::SubmitOrder(
    core::NativeOrderCommand command) {
    return impl_->order_execution.Submit(std::move(command));
}

std::expected<core::OrderCommandLookup, std::string>
TradingApplication::OrderCommandStatus(
    const std::string& request_id) {
    return impl_->order_execution.Lookup(request_id);
}

core::AccountActivityPage TradingApplication::AccountActivities(
    const core::AccountActivityQuery& query) const {
    return impl_->database.LoadAccountActivities(query);
}

void TradingApplication::RefreshAccountActivities() {
    impl_->broker.RequestAccountActivities();
}

core::RestTransportHealth TradingApplication::RestHealth() const {
    return impl_->broker.RestHealth();
}

core::MarketDataPipelineHealth
TradingApplication::MarketDataHealth() const {
    const auto storage =
        impl_->database.LoadMarketDataStorageUsage();
    const auto writer =
        impl_->database.WriterTelemetry();
    return {
        .candlestick_bytes = storage.candlestick_bytes,
        .tick_bytes = storage.tick_bytes,
        .database_bytes = storage.database_bytes,
        .candlestick_rows = storage.candlestick_rows,
        .tick_rows = storage.tick_rows,
        .pending_events = writer.pending_events,
        .high_water_events = writer.high_water_events,
        .dropped_market_events =
            writer.dropped_market_events,
        .pending_bars = writer.pending_bars,
        .high_water_bars = writer.high_water_bars,
        .dropped_bars = writer.dropped_bars,
        .persistence_failures = writer.write_failures,
        .last_persistence_error = writer.last_write_error,
        .retention_limited = false,
        .overloaded =
            writer.dropped_market_events != 0 ||
            writer.dropped_bars != 0 ||
            writer.write_failures != 0,
    };
}

void TradingApplication::RefreshMarketSymbols(
    const std::vector<std::string>& symbols) {
    static_cast<void>(UpdateMarketDataInterest({
        .consumer_id = "legacy.market-symbols",
        .feed = impl_->active_market_feed,
        .symbols = symbols,
        .priority = MarketDataInterestPriority::UserVisible,
    }));
}

std::expected<MarketDataSubscriptionPlan, std::string>
TradingApplication::UpdateMarketDataInterest(
    MarketDataInterest interest) {
    auto updated = impl_->market_interests.Upsert(std::move(interest));
    if (!updated) return updated;
    std::vector<std::string> planned =
        impl_->market_interests.Plan(
            impl_->active_market_feed).Symbols();
    if (planned != impl_->active_market_symbols) {
        impl_->active_market_symbols = planned;
        impl_->broker.RefreshSymbols(impl_->active_market_symbols);
    }
    return updated;
}

bool TradingApplication::RemoveMarketDataInterest(
    std::string_view consumer_id) {
    if (!impl_->market_interests.Remove(consumer_id)) return false;
    std::vector<std::string> planned =
        impl_->market_interests.Plan(
            impl_->active_market_feed).Symbols();
    if (planned != impl_->active_market_symbols) {
        impl_->active_market_symbols = planned;
        impl_->broker.RefreshSymbols(impl_->active_market_symbols);
    }
    return true;
}

MarketDataSubscriptionPlan TradingApplication::MarketDataSubscriptions(
    core::MarketDataFeed feed) const {
    return impl_->market_interests.Plan(feed);
}

void TradingApplication::RequestMarketHistory(
    const std::string& symbol, const std::string& timeframe) {
    impl_->broker.RequestHistory(symbol, timeframe);
}

void TradingApplication::RequestMarketHistory(
    const std::string& symbol, const std::string& timeframe,
    core::BarRange range) {
    impl_->broker.RequestHistory(symbol, timeframe, range);
}

void TradingApplication::RequestMarketHistory(
    core::HistoricalBarQuery query) {
    impl_->broker.RequestHistory(std::move(query));
}

void TradingApplication::RequestMarketHistory(
    const UiChartQuery& query) {
    impl_->broker.RequestHistory({
        .key = query.key,
        .symbol = query.symbol,
        .range = query.range,
    });
}

std::future<core::TickSeries> TradingApplication::RequestTicks(
    core::TickQuery query) {
    return impl_->broker.RequestTicks(std::move(query));
}

void TradingApplication::RefreshAssetCatalog() {
    impl_->broker.RequestAssetCatalog();
}

std::vector<UiEvent> TradingApplication::DrainUiEvents() {
    if (impl_->event_queue == nullptr) return {};
    std::vector<UiEvent> events = impl_->event_queue->Drain();
    for (const UiEvent& event : events)
        if (event.type == UiEventType::AssetCatalogReady) {
            impl_->database.SaveAssetCatalog(event.assets);
            std::unique_lock lock(impl_->asset_catalog_mutex);
            impl_->asset_catalog = event.assets;
        }
    return events;
}

}  // namespace tradebox::application
