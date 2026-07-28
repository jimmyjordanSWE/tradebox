#pragma once

#include "tradebox/core/market_data.h"
#include "tradebox/core/sequence_ring.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace tradebox::core {

class MarketDataStore final : public IMarketDataSink,
                              public IMarketDataView {
public:
    explicit MarketDataStore(
        std::size_t maximum_trades_per_symbol = 2'000,
        std::size_t maximum_changed_instruments = 65'536);

    using IMarketDataSink::Ingest;
    void Ingest(MarketDataEventPtr event) override;
    MarketDataSnapshot Snapshot(
        const std::string& symbol) const override;
    MarketDataDelta Delta(
        const std::string& symbol, std::uint64_t after_sequence,
        std::size_t maximum_events) const override;
    ChangedInstruments Changes(
        std::uint64_t after_sequence,
        std::size_t maximum_instruments) const override;

private:
    struct TradeSlot {
        MarketDataEventPtr owner;
        const MarketTrade* trade = nullptr;
        std::string identity;
        std::uint64_t receive_sequence = 0;
        bool corrected = false;
        bool active = false;
    };

    struct SymbolState {
        explicit SymbolState(std::size_t capacity)
            : trade_slots(capacity), events(capacity) {}

        std::string instrument_id;
        std::string symbol;
        MarketDataEventPtr quote_owner;
        const MarketQuote* quote = nullptr;
        std::vector<TradeSlot> trade_slots;
        std::size_t next_trade_slot = 0;
        std::size_t retained_trade_count = 0;
        std::unordered_map<std::string, std::size_t>
            trade_slot_by_identity;
        SequenceRing<MarketDataEventPtr> events;
        std::uint64_t revision = 0;
        std::uint64_t next_sequence = 1;
        std::uint64_t change_sequence = 0;
        std::int64_t last_received_at_ms = 0;
    };

    void Apply(const MarketDataEventPtr& owner,
               const QuoteReceived& event);
    void Apply(const MarketDataEventPtr& owner,
               const TradeReceived& event);
    void Apply(const MarketDataEventPtr& owner,
               const TradeCanceled& event);
    void Apply(const MarketDataEventPtr& owner,
               const TradeCorrected& event);
    void Apply(const MarketDataEventPtr& owner,
               const MarketStreamChanged& event);
    SymbolState& StateFor(std::string_view instrument_id,
                          std::string_view symbol);
    const SymbolState* FindState(std::string_view identifier) const;
    void RecordChange(SymbolState& state);
    bool InsertTrade(SymbolState& state,
                     MarketDataEventPtr owner,
                     const MarketTrade* trade,
                     bool corrected = false);
    bool EraseTrade(SymbolState& state,
                    std::string_view trade_id,
                    std::int64_t event_time_ns);
    void AppendEvent(SymbolState& state,
                     MarketDataEventPtr event);

    const std::size_t maximum_trades_per_symbol_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SymbolState*> symbols_;
    std::vector<std::unique_ptr<SymbolState>> state_storage_;
    std::unordered_map<std::string, std::string> keys_by_symbol_;
    std::uint64_t next_change_sequence_ = 1;
    std::unordered_set<std::string> trade_subscriptions_;
    std::unordered_set<std::string> quote_subscriptions_;
    MarketDataFeed feed_ = MarketDataFeed::Unknown;
    MarketStreamStatus stream_status_ =
        MarketStreamStatus::Disconnected;
    std::string status_message_ = "Disconnected";
    std::uint64_t next_receive_sequence_ = 1;
    SequenceRing<SymbolState*> changes_;
};

}  // namespace tradebox::core
