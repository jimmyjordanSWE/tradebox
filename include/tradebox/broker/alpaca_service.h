#pragma once

#include "tradebox/broker/gateway.h"
#include "tradebox/core/interfaces.h"
#include "tradebox/core/market_data.h"
#include "tradebox/persistence/database.h"
#include "tradebox/platform/credentials.h"
#include "tradebox/ui/model.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <winhttp.h>

class AlpacaService : public tradebox::broker::IOrderGateway {
public:
    AlpacaService(UiEventQueue& events, Database& database,
                  tradebox::core::ITradingCore& core,
                  tradebox::core::IMarketDataSink& market_data);
    ~AlpacaService();

    AlpacaService(const AlpacaService&) = delete;
    AlpacaService& operator=(const AlpacaService&) = delete;

    void Connect(AlpacaCredentials credentials,
                 const std::vector<std::string>& symbols,
                 tradebox::core::MarketDataFeed feed =
                     tradebox::core::MarketDataFeed::Iex);
    void RefreshSymbols(const std::vector<std::string>& symbols);
    void RequestHistory(const std::string& symbol);
    void Disconnect();
    bool Connected() const { return connected_; }
    bool AccountConnected() const { return account_connected_; }
    tradebox::broker::BrokerCommandResult PlaceOrder(
        const tradebox::core::NativeOrderRequest& request) override;
    tradebox::broker::BrokerCommandResult CancelOrder(
        const std::string& order_id) override;
    tradebox::broker::BrokerCommandResult ReplaceOrder(
        const std::string& order_id,
        const tradebox::core::ReplaceOrderRequest& request) override;

private:
    void FetchAccount();
    void FetchPositions();
    void FetchOrders();
    void AccountRefreshLoop();
    void FetchMarketClock();
    void MarketClockLoop();
    void FetchHistory(std::string symbol);
    void StreamLoop(std::vector<std::string> symbols);
    void AccountStreamLoop();
    void JoinWorkers();
    void PublishCoreEvent(tradebox::core::BrokerEvent event);
    AlpacaCredentials CredentialsSnapshot() const;

    UiEventQueue& events_;
    Database& database_;
    tradebox::core::ITradingCore& core_;
    tradebox::core::IMarketDataSink& market_data_;
    AlpacaCredentials credentials_;
    mutable std::mutex credentials_mutex_;
    mutable std::recursive_mutex lifecycle_mutex_;
    std::atomic<bool> running_ = false;
    std::atomic<bool> connected_ = false;
    std::atomic<HINTERNET> websocket_ = nullptr;
    std::atomic<HINTERNET> account_websocket_ = nullptr;
    std::atomic<bool> account_connected_ = false;
    std::atomic<bool> orders_dirty_ = true;
    std::atomic<bool> positions_dirty_ = true;
    std::atomic<std::uint64_t> generation_counter_ = 0;
    std::atomic<std::uint64_t> active_generation_ = 0;
    tradebox::core::MarketDataFeed market_data_feed_ =
        tradebox::core::MarketDataFeed::Iex;
    std::mutex subscription_mutex_;
    std::mutex websocket_send_mutex_;
    std::vector<std::string> desired_symbols_;
    std::vector<std::string> subscribed_symbols_;
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
    std::thread stream_thread_;
    std::thread account_stream_thread_;
};
