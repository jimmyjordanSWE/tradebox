#pragma once

#include "tradebox/core/order_command.h"
#include "tradebox/core/account_activity.h"
#include "tradebox/core/bar_series.h"
#include "tradebox/core/market_data.h"
#include "tradebox/core/types.h"
#include "tradebox/core/rest_health.h"
#include "tradebox/application/ui_snapshot.h"

#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Database;
class UiEventQueue;

namespace tradebox::application {

struct ConnectionRequest {
    core::AccountEnvironment environment = core::AccountEnvironment::Paper;
    std::string api_key;
    std::string api_secret;
    std::vector<std::string> market_symbols;
    core::MarketDataFeed market_data_feed = core::MarketDataFeed::Iex;
};

class TradingApplication final {
public:
    explicit TradingApplication(Database& database);
    TradingApplication(
        Database& database,
        core::IOrderCommandJournal& order_journal);
    TradingApplication(UiEventQueue& events, Database& database);
    ~TradingApplication();

    TradingApplication(const TradingApplication&) = delete;
    TradingApplication& operator=(const TradingApplication&) = delete;

    [[nodiscard]] core::CoreSnapshot Snapshot() const;
    [[nodiscard]] ApplicationUiSnapshot SnapshotForUi(
        const UiSnapshotQuery& query) const;
    [[nodiscard]] std::optional<core::CoreSnapshot> SnapshotAfter(
        std::uint64_t revision) const;
    [[nodiscard]] core::MarketDataSnapshot MarketData(
        const std::string& symbol) const;
    [[nodiscard]] core::MarketDataDelta MarketDataChanges(
        const std::string& symbol, std::uint64_t after_sequence,
        std::size_t maximum_events = 512) const;
    [[nodiscard]] core::ChangedInstruments ChangedMarketInstruments(
        std::uint64_t after_sequence,
        std::size_t maximum_instruments = 4096) const;
    [[nodiscard]] core::BarSeriesSnapshot Bars(
        const core::BarSeriesKey& key,
        core::BarRange range) const;
    [[nodiscard]] core::BarSeriesSnapshot BarsForSymbol(
        const std::string& symbol, const std::string& timeframe,
        core::BarRange range) const;
    [[nodiscard]] core::ChangedBarSeriesBatch ChangedBarSeries(
        std::uint64_t after_sequence,
        std::size_t maximum_series = 512) const;
    [[nodiscard]] std::expected<core::CommandReceipt, core::CoreError>
    Connect(ConnectionRequest request);
    [[nodiscard]] std::expected<core::CommandReceipt, core::CoreError>
    Disconnect();
    [[nodiscard]] std::future<core::OrderCommandResult> SubmitOrder(
        core::NativeOrderCommand command);
    [[nodiscard]] std::expected<core::OrderCommandLookup, std::string>
    OrderCommandStatus(const std::string& request_id);
    [[nodiscard]] core::AccountActivityPage AccountActivities(
        const core::AccountActivityQuery& query) const;
    void RefreshAccountActivities();
    [[nodiscard]] core::RestTransportHealth RestHealth() const;
    [[nodiscard]] core::MarketDataPipelineHealth
    MarketDataHealth() const;

    void RefreshMarketSymbols(const std::vector<std::string>& symbols);
    void RequestMarketHistory(const std::string& symbol,
                              const std::string& timeframe = "1Day");
    void RequestMarketHistory(const std::string& symbol,
                              const std::string& timeframe,
                              core::BarRange range);
    void RequestMarketHistory(core::HistoricalBarQuery query);
    void RequestMarketHistory(const UiChartQuery& query);
    [[nodiscard]] std::future<core::TickSeries> RequestTicks(
        core::TickQuery query);
    void RefreshAssetCatalog();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tradebox::application
