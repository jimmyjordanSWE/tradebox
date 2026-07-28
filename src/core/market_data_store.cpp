#include "tradebox/core/market_data_store.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace tradebox::core {

MarketDataStore::MarketDataStore(
    std::size_t maximum_trades_per_symbol)
    : maximum_trades_per_symbol_(maximum_trades_per_symbol) {}

void MarketDataStore::Ingest(MarketDataEvent event) {
    std::scoped_lock lock(mutex_);
    std::visit(
        [this](auto typed) { Apply(std::move(typed)); },
        std::move(event));
}

MarketDataSnapshot MarketDataStore::Snapshot(
    const std::string& symbol) const {
    std::scoped_lock lock(mutex_);
    MarketDataSnapshot result{
        .symbol = symbol,
        .feed = feed_,
        .stream_status = stream_status_,
        .trades_subscribed = trade_subscriptions_.contains(symbol),
        .quotes_subscribed = quote_subscriptions_.contains(symbol),
        .status_message = status_message_,
    };
    const auto found = symbols_.find(symbol);
    if (found == symbols_.end()) return result;
    result.latest_quote = found->second.quote;
    result.trades = found->second.trades;
    result.revision = found->second.revision;
    result.last_received_at_ms = found->second.last_received_at_ms;
    return result;
}

void MarketDataStore::Apply(QuoteReceived event) {
    SymbolState& state = symbols_[event.quote.symbol];
    if (state.quote && event.quote.event_time_ns > 0 &&
        state.quote->event_time_ns > 0 &&
        event.quote.event_time_ns < state.quote->event_time_ns)
        return;
    state.last_received_at_ms =
        std::max(state.last_received_at_ms,
                 event.quote.received_at_ms);
    state.quote = std::move(event.quote);
    ++state.revision;
}

void MarketDataStore::Apply(TradeReceived event) {
    SymbolState& state = symbols_[event.trade.symbol];
    event.trade.receive_sequence = next_receive_sequence_++;
    state.last_received_at_ms =
        std::max(state.last_received_at_ms,
                 event.trade.received_at_ms);
    const auto duplicate = std::ranges::find_if(
        state.trades, [&event](const MarketTrade& trade) {
            return !event.trade.trade_id.empty() &&
                   trade.trade_id == event.trade.trade_id;
        });
    if (duplicate != state.trades.end()) return;
    InsertTrade(state, std::move(event.trade));
    ++state.revision;
}

void MarketDataStore::Apply(TradeCanceled event) {
    SymbolState& state = symbols_[event.symbol];
    const auto old_size = state.trades.size();
    std::erase_if(state.trades, [&event](const MarketTrade& trade) {
        return trade.trade_id == event.trade_id;
    });
    if (state.trades.size() != old_size) ++state.revision;
}

void MarketDataStore::Apply(TradeCorrected event) {
    SymbolState& state = symbols_[event.symbol];
    std::erase_if(state.trades, [&event](const MarketTrade& trade) {
        return trade.trade_id == event.original_trade_id;
    });
    event.corrected_trade.corrected = true;
    event.corrected_trade.receive_sequence =
        next_receive_sequence_++;
    state.last_received_at_ms =
        std::max(state.last_received_at_ms,
                 event.corrected_trade.received_at_ms);
    InsertTrade(state, std::move(event.corrected_trade));
    ++state.revision;
}

void MarketDataStore::Apply(MarketStreamChanged event) {
    feed_ = event.feed;
    stream_status_ = event.status;
    status_message_ = std::move(event.message);
    trade_subscriptions_.clear();
    quote_subscriptions_.clear();
    trade_subscriptions_.insert(event.trade_symbols.begin(),
                                event.trade_symbols.end());
    quote_subscriptions_.insert(event.quote_symbols.begin(),
                                event.quote_symbols.end());
    for (auto& [symbol, state] : symbols_) {
        static_cast<void>(symbol);
        ++state.revision;
    }
}

void MarketDataStore::InsertTrade(SymbolState& state,
                                  MarketTrade trade) {
    const auto position = std::ranges::find_if(
        state.trades, [&trade](const MarketTrade& existing) {
            if (existing.event_time_ns != trade.event_time_ns)
                return existing.event_time_ns < trade.event_time_ns;
            return existing.receive_sequence <
                   trade.receive_sequence;
        });
    state.trades.insert(position, std::move(trade));
    if (state.trades.size() > maximum_trades_per_symbol_)
        state.trades.resize(maximum_trades_per_symbol_);
}

}  // namespace tradebox::core
