#pragma once

#include "tradebox/broker/gateway.h"
#include "tradebox/broker/alpaca_bar_request.h"
#include "tradebox/broker/alpaca_rest_transport.h"
#include "tradebox/broker/alpaca_stream_supervision.h"
#include "tradebox/core/interfaces.h"
#include "tradebox/core/bar_series.h"
#include "tradebox/core/market_data.h"
#include "tradebox/persistence/database.h"
#include "tradebox/platform/credentials.h"
#include "tradebox/ui/model.h"

#include <atomic>
#include <future>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <winhttp.h>

class AlpacaService : public tradebox::broker::IOrderGateway {
public:
    AlpacaService(UiEventQueue& events, Database& database,
                  tradebox::core::ITradingCore& core,
                  tradebox::core::IMarketDataSink& market_data,
                  tradebox::core::IMarketDataView& market_data_view,
                  tradebox::core::IBarDataSink& bars);
    ~AlpacaService();

    AlpacaService(const AlpacaService&) = delete;
    AlpacaService& operator=(const AlpacaService&) = delete;

    void Connect(AlpacaCredentials credentials,
                 const std::vector<std::string>& symbols,
                 tradebox::core::MarketDataFeed feed =
                     tradebox::core::MarketDataFeed::Iex);
    void RefreshSymbols(const std::vector<std::string>& symbols);
    void RequestHistory(const std::string& symbol,
                        const std::string& timeframe = "1Day");
    void RequestHistory(const std::string& symbol,
                        const std::string& timeframe,
                        tradebox::core::BarRange range);
    void RequestHistory(
        tradebox::core::HistoricalBarQuery query);
    [[nodiscard]] tradebox::core::BarSeriesKey ResolveBarSeriesKey(
        const std::string& symbol,
        const std::string& timeframe) const;
    void RequestAssetCatalog();
    void RequestAccountActivities();
    std::future<tradebox::core::TickSeries> RequestTicks(
        tradebox::core::TickQuery query);
    void Disconnect();
    bool Connected() const { return connected_; }
    bool AccountConnected() const { return account_connected_; }
    tradebox::core::RestTransportHealth RestHealth() const;
    tradebox::broker::BrokerCommandResult PlaceOrder(
        const tradebox::core::NativeOrderRequest& request) override;
    tradebox::broker::BrokerCommandResult CancelOrder(
        const std::string& order_id) override;
    tradebox::broker::BrokerCommandResult ReplaceOrder(
        const std::string& order_id,
        const tradebox::core::ReplaceOrderRequest& request) override;
    tradebox::broker::BrokerCommandResult ClosePosition(
        const std::string& symbol_or_asset_id,
        const std::optional<tradebox::core::Decimal>& qty,
        const std::optional<tradebox::core::Decimal>& percentage) override;
    tradebox::broker::BrokerCommandResult CloseAllPositions(
        bool cancel_open_orders) override;
    tradebox::broker::BrokerCommandResult CancelAllOrders() override;

private:
    void FetchAccount();
    void FetchPositions();
    void FetchOrders();
    void FetchAccountActivities();
    void AccountRefreshLoop();
    void FetchMarketClock();
    void MarketClockLoop();
    void FetchHistory(
        std::string symbol,
        tradebox::core::BarSeriesKey key,
        tradebox::core::BarRange requested_range,
        std::vector<tradebox::core::BarRange> reserved_ranges);
    void PublishCachedHistory(
        const std::string& symbol,
        const tradebox::core::BarSeriesKey& key,
        tradebox::core::BarRange range);
    void FetchAssetCatalog();
    void SeedLatestSnapshots(std::vector<std::string> symbols);
    tradebox::core::TickSeries FetchTicks(
        const tradebox::core::TickQuery& query);
    void StreamLoop();
    bool RunMarketStreamSession();
    void AccountStreamLoop();
    bool RunAccountStreamSession();
    bool WaitForReconnect(std::chrono::milliseconds delay);
    void ScheduleMarketGapBackfill(
        std::int64_t disconnected_at_ns,
        std::int64_t reconnected_at_ns,
        std::vector<std::string> symbols);
    void JoinWorkers();
    bool SubmitBackground(std::function<void()> task);
    void BackgroundWorkerLoop();
    void WaitForBackgroundIdle();
    void ReportPersistenceHealth();
    void PublishCoreEvent(tradebox::core::BrokerEvent event);
    AlpacaCredentials CredentialsSnapshot() const;
    std::string InstrumentIdForSymbol(const std::string& symbol) const;

    UiEventQueue& events_;
    Database& database_;
    tradebox::core::ITradingCore& core_;
    tradebox::core::IMarketDataSink& market_data_;
    tradebox::core::IMarketDataView& market_data_view_;
    tradebox::core::IBarDataSink& bars_;
    tradebox::broker::alpaca::AlpacaRestTransport rest_transport_;
    AlpacaCredentials credentials_;
    mutable std::mutex credentials_mutex_;
    mutable std::recursive_mutex lifecycle_mutex_;
    std::atomic<bool> running_ = false;
    std::atomic<bool> connected_ = false;
    std::atomic<HINTERNET> websocket_ = nullptr;
    std::atomic<HINTERNET> account_websocket_ = nullptr;
    std::atomic<bool> account_connected_ = false;
    std::atomic<bool> account_dirty_ = true;
    std::atomic<bool> orders_dirty_ = true;
    std::atomic<bool> positions_dirty_ = true;
    std::atomic<bool> activities_dirty_ = true;
    bool activities_identity_deferred_reported_ = false;
    std::atomic<std::uint64_t> generation_counter_ = 0;
    std::atomic<std::uint64_t> active_generation_ = 0;
    std::atomic<std::uint64_t> account_stream_attempt_ = 0;
    std::atomic<std::int64_t> market_gap_started_ns_ = 0;
    tradebox::core::MarketDataFeed market_data_feed_ =
        tradebox::core::MarketDataFeed::Iex;
    std::mutex subscription_mutex_;
    mutable std::mutex asset_catalog_mutex_;
    std::unordered_map<std::string, std::string> instrument_ids_by_symbol_;
    std::mutex websocket_send_mutex_;
    std::vector<std::string> desired_symbols_;
    std::vector<std::string> subscribed_symbols_;
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
    mutable std::mutex background_mutex_;
    std::condition_variable background_ready_;
    std::condition_variable background_idle_;
    std::deque<std::function<void()>> background_tasks_;
    std::vector<std::thread> background_workers_;
    std::size_t background_active_ = 0;
    std::uint64_t background_rejected_ = 0;
    std::atomic<std::uint64_t> tick_requests_coalesced_ = 0;
    std::atomic<bool> persistence_queue_warning_emitted_ = false;
    std::uint64_t reported_persistence_failures_ = 0;
    bool background_stopping_ = false;
    std::mutex tick_requests_mutex_;
    std::unordered_map<
        std::string,
        std::vector<std::shared_ptr<
            std::promise<tradebox::core::TickSeries>>>>
        tick_requests_;
    std::thread stream_thread_;
    std::thread account_stream_thread_;
    tradebox::broker::alpaca::InFlightBarRanges
        in_flight_bar_ranges_;
};
