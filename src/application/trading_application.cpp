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
#include <ranges>
#include <type_traits>
#include <utility>

namespace tradebox::application {
namespace {

void ClearSensitiveString(std::string& value) noexcept {
    volatile char* bytes = value.data();
    for (std::size_t index = 0; index < value.size(); ++index)
        bytes[index] = '\0';
    value.clear();
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
          journal(database),
          order_journal(database),
          core(journal, clock),
          valuation_market_data(market_data, core),
          broker(*owned_events, database, core,
                 valuation_market_data,
                 market_data, bars, &history_requests),
          order_execution(core, broker, order_journal, clock,
                          &market_data) {}

    Impl(Database& database,
         core::IOrderCommandJournal& external_order_journal)
        : database(database),
          owned_events(std::make_unique<UiEventQueue>()),
          journal(database),
          order_journal(database),
          core(journal, clock),
          valuation_market_data(market_data, core),
          broker(*owned_events, database, core,
                 valuation_market_data,
                 market_data, bars, &history_requests),
          order_execution(core, broker, external_order_journal, clock,
                          &market_data) {}

    Impl(UiEventQueue& events, Database& database)
        : database(database),
          journal(database),
          order_journal(database),
          core(journal, clock),
          valuation_market_data(market_data, core),
          broker(events, database, core,
                 valuation_market_data,
                 market_data, bars, &history_requests),
          order_execution(core, broker, order_journal, clock,
                          &market_data) {}

    Database& database;
    std::unique_ptr<UiEventQueue> owned_events;
    persistence::DatabaseEventJournal journal;
    persistence::DatabaseOrderCommandJournal order_journal;
    core::SystemClock clock;
    core::TradingCore core;
    core::MarketDataStore market_data;
    CoreValuationMarketDataSink valuation_market_data;
    core::BarStore bars;
    HistoryRequestTracker history_requests;
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
    result.markets.reserve(query.market_symbols.size());
    for (const std::string& symbol : query.market_symbols)
        result.markets.push_back(MarketData(symbol));

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

        projected.series = Bars(chart.key, chart.range);
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

        std::vector<core::MarketBar> indicator_bars =
            projected.series.bars;
        if (projected.series.current_bar) {
            const auto position = std::ranges::lower_bound(
                indicator_bars, projected.series.current_bar->start_ns,
                {}, &core::MarketBar::start_ns);
            if (position != indicator_bars.end() &&
                position->start_ns ==
                    projected.series.current_bar->start_ns)
                *position = *projected.series.current_bar;
            else
                indicator_bars.insert(
                    position, *projected.series.current_bar);
        }
        auto indicators = core::EvaluateIndicators(
            chart.indicators, indicator_bars);
        if (indicators)
            projected.indicators = std::move(*indicators);
        else
            projected.indicator_errors.push_back(
                indicators.error().message);
        result.charts.push_back(std::move(projected));
    }
    if (query.asset_limit != 0) {
        const auto assets = impl_->database.LoadAssetCatalog();
        result.assets = core::SearchTradableAssets(
            assets, query.asset_search, query.asset_limit);
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

    const std::string identifier =
        key.instrument_id.empty() ? snapshot.symbol
                                  : key.instrument_id;
    const core::MarketDataSnapshot live =
        impl_->market_data.Snapshot(identifier);
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
    auto receipt = impl_->core.Submit(
        core::ConnectAccount{request.environment});
    if (!receipt || !receipt->accepted) return receipt;

    AlpacaCredentials credentials(
        request.api_key, request.api_secret,
        request.environment == core::AccountEnvironment::Paper);
    ClearSensitiveString(request.api_key);
    ClearSensitiveString(request.api_secret);
    impl_->broker.Connect(std::move(credentials),
                          request.market_symbols,
                          request.market_data_feed);
    return receipt;
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
    impl_->broker.RefreshSymbols(symbols);
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

}  // namespace tradebox::application
