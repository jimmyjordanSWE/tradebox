#include "tradebox/core/market_data_store.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace tradebox::core {

namespace {

constexpr std::int64_t kDayNs =
    24LL * 60 * 60 * 1'000'000'000;

bool SameTradeIdentity(const MarketTrade& left,
                       const MarketTrade& right) {
    if (left.trade_id.empty() ||
        left.trade_id != right.trade_id)
        return false;
    if (left.event_time_ns <= 0 || right.event_time_ns <= 0)
        return true;
    return left.event_time_ns / kDayNs ==
           right.event_time_ns / kDayNs;
}

bool EraseTradeIdentity(std::vector<MarketTrade>& trades,
                        std::string_view trade_id,
                        std::int64_t event_time_ns) {
    const std::size_t previous_size = trades.size();
    std::erase_if(trades, [&](const MarketTrade& trade) {
        return trade.trade_id == trade_id &&
               (event_time_ns <= 0 || trade.event_time_ns <= 0 ||
                trade.event_time_ns / kDayNs ==
                    event_time_ns / kDayNs);
    });
    if (trades.size() != previous_size) return true;
    const auto fallback = std::ranges::find_if(
        trades, [&](const MarketTrade& trade) {
            return trade.trade_id == trade_id;
        });
    if (fallback == trades.end()) return false;
    trades.erase(fallback);
    return true;
}

}  // namespace

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

MarketDataDelta MarketDataStore::Delta(
    const std::string& symbol, std::uint64_t after_sequence,
    std::size_t maximum_events) const {
    std::scoped_lock lock(mutex_);
    MarketDataDelta result{
        .symbol = symbol,
        .feed = feed_,
        .stream_status = stream_status_,
        .trades_subscribed = trade_subscriptions_.contains(symbol),
        .quotes_subscribed = quote_subscriptions_.contains(symbol),
        .status_message = status_message_,
    };
    const auto found = symbols_.find(symbol);
    if (found == symbols_.end()) return result;
    const SymbolState& state = found->second;
    result.latest_quote = state.quote;
    result.next_sequence = state.next_sequence - 1;
    result.last_received_at_ms = state.last_received_at_ms;
    if (state.events.empty() || maximum_events == 0) return result;
    const std::uint64_t oldest = state.events.front().sequence;
    result.gap_detected =
        after_sequence != 0 && after_sequence + 1 < oldest;
    for (const SequencedMarketDataEvent& event : state.events) {
        if (event.sequence <= after_sequence) continue;
        if (result.events.size() >= maximum_events) break;
        result.events.push_back(event);
    }
    if (!result.events.empty())
        result.next_sequence = result.events.back().sequence;
    else
        result.next_sequence = std::max(
            after_sequence, state.next_sequence - 1);
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
    state.quote = event.quote;
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
            return SameTradeIdentity(trade, event.trade);
        });
    if (duplicate != state.trades.end()) return;
    InsertTrade(state, event.trade);
    ++state.revision;
    AppendEvent(state, std::move(event));
}

void MarketDataStore::Apply(TradeCanceled event) {
    SymbolState& state = symbols_[event.symbol];
    if (EraseTradeIdentity(
            state.trades, event.trade_id, event.event_time_ns))
        ++state.revision;
    AppendEvent(state, std::move(event));
}

void MarketDataStore::Apply(TradeCorrected event) {
    SymbolState& state = symbols_[event.symbol];
    EraseTradeIdentity(
        state.trades, event.original_trade_id,
        event.corrected_trade.event_time_ns);
    event.corrected_trade.corrected = true;
    event.corrected_trade.receive_sequence =
        next_receive_sequence_++;
    state.last_received_at_ms =
        std::max(state.last_received_at_ms,
                 event.corrected_trade.received_at_ms);
    InsertTrade(state, event.corrected_trade);
    ++state.revision;
    AppendEvent(state, std::move(event));
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

void MarketDataStore::AppendEvent(SymbolState& state,
                                  MarketDataEvent event) {
    state.events.push_back({
        .sequence = state.next_sequence++,
        .event = std::move(event),
    });
    while (state.events.size() > maximum_trades_per_symbol_)
        state.events.pop_front();
}

}  // namespace tradebox::core
