#pragma once

#include "tradebox/core/order_command.h"
#include "tradebox/core/market_data.h"
#include "tradebox/core/types.h"

#include <expected>
#include <future>
#include <memory>
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
    TradingApplication(UiEventQueue& events, Database& database);
    ~TradingApplication();

    TradingApplication(const TradingApplication&) = delete;
    TradingApplication& operator=(const TradingApplication&) = delete;

    [[nodiscard]] core::CoreSnapshot Snapshot() const;
    [[nodiscard]] core::MarketDataSnapshot MarketData(
        const std::string& symbol) const;
    [[nodiscard]] std::expected<core::CommandReceipt, core::CoreError>
    Connect(ConnectionRequest request);
    [[nodiscard]] std::expected<core::CommandReceipt, core::CoreError>
    Disconnect();
    [[nodiscard]] std::future<core::OrderCommandResult> SubmitOrder(
        core::NativeOrderCommand command);
    [[nodiscard]] std::expected<core::OrderCommandLookup, std::string>
    OrderCommandStatus(const std::string& request_id);

    void RefreshMarketSymbols(const std::vector<std::string>& symbols);
    void RequestMarketHistory(const std::string& symbol);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tradebox::application
