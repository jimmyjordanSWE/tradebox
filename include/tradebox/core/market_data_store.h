#pragma once

#include "tradebox/core/market_data.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace tradebox::core {

class MarketDataStore final : public IMarketDataSink,
                              public IMarketDataView {
public:
    explicit MarketDataStore(std::size_t maximum_trades_per_symbol = 2'000);

    void Ingest(MarketDataEvent event) override;
    MarketDataSnapshot Snapshot(
        const std::string& symbol) const override;
    MarketDataDelta Delta(
        const std::string& symbol, std::uint64_t after_sequence,
        std::size_t maximum_events) const override;

private:
    struct SymbolState {
        std::optional<MarketQuote> quote;
        std::vector<MarketTrade> trades;
        std::deque<SequencedMarketDataEvent> events;
        std::uint64_t revision = 0;
        std::uint64_t next_sequence = 1;
        std::int64_t last_received_at_ms = 0;
    };

    void Apply(QuoteReceived event);
    void Apply(TradeReceived event);
    void Apply(TradeCanceled event);
    void Apply(TradeCorrected event);
    void Apply(MarketStreamChanged event);
    void InsertTrade(SymbolState& state, MarketTrade trade);
    void AppendEvent(SymbolState& state, MarketDataEvent event);

    const std::size_t maximum_trades_per_symbol_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SymbolState> symbols_;
    std::unordered_set<std::string> trade_subscriptions_;
    std::unordered_set<std::string> quote_subscriptions_;
    MarketDataFeed feed_ = MarketDataFeed::Unknown;
    MarketStreamStatus stream_status_ =
        MarketStreamStatus::Disconnected;
    std::string status_message_ = "Disconnected";
    std::uint64_t next_receive_sequence_ = 1;
};

}  // namespace tradebox::core
