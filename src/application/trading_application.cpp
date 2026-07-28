#include "tradebox/application/trading_application.h"

#include "tradebox/application/order_execution_service.h"
#include "tradebox/broker/alpaca_service.h"
#include "tradebox/core/system_clock.h"
#include "tradebox/core/market_data_store.h"
#include "tradebox/core/trading_core.h"
#include "tradebox/persistence/database_event_journal.h"
#include "tradebox/persistence/database_order_command_journal.h"
#include "tradebox/platform/credentials.h"

#include <utility>

namespace tradebox::application {

class TradingApplication::Impl final {
public:
    explicit Impl(Database& database)
        : owned_events(std::make_unique<UiEventQueue>()),
          journal(database),
          order_journal(database),
          core(journal, clock),
          broker(*owned_events, database, core, market_data),
          order_execution(core, broker, order_journal, clock) {}

    Impl(UiEventQueue& events, Database& database)
        : journal(database),
          order_journal(database),
          core(journal, clock),
          broker(events, database, core, market_data),
          order_execution(core, broker, order_journal, clock) {}

    std::unique_ptr<UiEventQueue> owned_events;
    persistence::DatabaseEventJournal journal;
    persistence::DatabaseOrderCommandJournal order_journal;
    core::SystemClock clock;
    core::TradingCore core;
    core::MarketDataStore market_data;
    AlpacaService broker;
    OrderExecutionService order_execution;
};

TradingApplication::TradingApplication(Database& database)
    : impl_(std::make_unique<Impl>(database)) {}

TradingApplication::TradingApplication(UiEventQueue& events,
                                       Database& database)
    : impl_(std::make_unique<Impl>(events, database)) {}

TradingApplication::~TradingApplication() = default;

core::CoreSnapshot TradingApplication::Snapshot() const {
    return impl_->core.Snapshot();
}

core::MarketDataSnapshot TradingApplication::MarketData(
    const std::string& symbol) const {
    return impl_->market_data.Snapshot(symbol);
}

std::expected<core::CommandReceipt, core::CoreError>
TradingApplication::Connect(ConnectionRequest request) {
    auto receipt = impl_->core.Submit(
        core::ConnectAccount{request.environment});
    if (!receipt || !receipt->accepted) return receipt;

    AlpacaCredentials credentials{
        .key = std::move(request.api_key),
        .secret = std::move(request.api_secret),
        .paper = request.environment == core::AccountEnvironment::Paper,
    };
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

void TradingApplication::RefreshMarketSymbols(
    const std::vector<std::string>& symbols) {
    impl_->broker.RefreshSymbols(symbols);
}

void TradingApplication::RequestMarketHistory(
    const std::string& symbol) {
    impl_->broker.RequestHistory(symbol);
}

}  // namespace tradebox::application
