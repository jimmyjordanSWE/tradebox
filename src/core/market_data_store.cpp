#include "tradebox/core/market_data_store.h"

#include <algorithm>
#include <utility>

namespace tradebox::core {
namespace {

constexpr std::int64_t kDayNs =
    24LL * 60 * 60 * 1'000'000'000;

std::string TradeIdentity(std::string_view trade_id,
                          std::int64_t event_time_ns) {
    if (trade_id.empty()) return {};
    if (event_time_ns <= 0)
        return std::string(trade_id) + "@?";
    return std::string(trade_id) + "@" +
           std::to_string(event_time_ns / kDayNs);
}

bool Newer(const MarketTrade& left, std::uint64_t left_sequence,
           const MarketTrade& right, std::uint64_t right_sequence) {
    if (left.event_time_ns != right.event_time_ns)
        return left.event_time_ns > right.event_time_ns;
    return left_sequence > right_sequence;
}

}  // namespace

MarketDataStore::MarketDataStore(
    std::size_t maximum_trades_per_symbol,
    std::size_t maximum_changed_instruments)
    : maximum_trades_per_symbol_(
          std::max<std::size_t>(maximum_trades_per_symbol, 1)),
      changes_(maximum_changed_instruments) {}

void MarketDataStore::Ingest(MarketDataEventPtr event) {
    if (!event) return;
    std::scoped_lock lock(mutex_);
    std::visit(
        [this, &event](const auto& typed) {
            Apply(event, typed);
        },
        *event);
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
    const SymbolState* state = FindState(symbol);
    if (!state) return result;
    result.instrument_id = state->instrument_id;
    result.symbol = state->symbol;
    result.trades_subscribed =
        trade_subscriptions_.contains(state->symbol);
    result.quotes_subscribed =
        quote_subscriptions_.contains(state->symbol);
    if (state->quote) result.latest_quote = *state->quote;

    std::vector<const TradeSlot*> retained;
    retained.reserve(state->retained_trade_count);
    for (const TradeSlot& slot : state->trade_slots)
        if (slot.active && slot.trade)
            retained.push_back(&slot);
    std::ranges::sort(
        retained, [](const TradeSlot* left,
                     const TradeSlot* right) {
            return Newer(*left->trade, left->receive_sequence,
                         *right->trade, right->receive_sequence);
        });
    for (const TradeSlot* slot : retained) {
        MarketTrade trade = *slot->trade;
        trade.receive_sequence = slot->receive_sequence;
        trade.corrected = slot->corrected;
        result.trades.push_back(std::move(trade));
    }
    result.revision = state->revision;
    result.last_received_at_ms = state->last_received_at_ms;
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
    const SymbolState* found = FindState(symbol);
    if (!found) return result;
    const SymbolState& state = *found;
    result.instrument_id = state.instrument_id;
    result.symbol = state.symbol;
    result.trades_subscribed =
        trade_subscriptions_.contains(state.symbol);
    result.quotes_subscribed =
        quote_subscriptions_.contains(state.symbol);
    result.next_sequence = state.next_sequence - 1;
    result.last_received_at_ms = state.last_received_at_ms;
    if (state.events.Empty() || maximum_events == 0) return result;
    const std::uint64_t oldest = state.events.Oldest();
    result.gap_detected =
        after_sequence != 0 && after_sequence + 1 < oldest;
    result.events.reserve(
        std::min(maximum_events, state.events.Size()));
    state.events.VisitAfter(
        after_sequence, maximum_events,
        [&](const auto& entry) {
            result.events.push_back({
                .sequence = entry.sequence,
                .event = entry.value,
            });
        });
    if (!result.events.empty())
        result.next_sequence = result.events.back().sequence;
    else
        result.next_sequence = std::max(
            after_sequence, state.next_sequence - 1);
    return result;
}

ChangedInstruments MarketDataStore::Changes(
    std::uint64_t after_sequence,
    std::size_t maximum_instruments) const {
    std::scoped_lock lock(mutex_);
    ChangedInstruments result{
        .next_sequence = after_sequence,
    };
    if (maximum_instruments == 0 || changes_.Empty())
        return result;
    result.gap_detected =
        after_sequence != 0 &&
        after_sequence + 1 < changes_.Oldest();
    std::unordered_map<std::string, ChangedInstrument> latest;
    latest.reserve(
        std::min(maximum_instruments, changes_.Size()));
    changes_.VisitAfter(
        after_sequence, changes_.Size(),
        [&](const auto& entry) {
            const SymbolState* state = entry.value;
            if (!state) return;
            ChangedInstrument change{
                .sequence = entry.sequence,
                .instrument_id = state->instrument_id,
                .symbol = state->symbol,
            };
            const std::string key =
                change.instrument_id.empty()
                    ? change.symbol
                    : change.instrument_id;
            latest.insert_or_assign(key, std::move(change));
        });
    result.instruments.reserve(
        std::min(maximum_instruments, latest.size()));
    for (auto& [key, change] : latest) {
        static_cast<void>(key);
        result.instruments.push_back(std::move(change));
    }
    std::ranges::sort(result.instruments, {},
                      &ChangedInstrument::sequence);
    if (result.instruments.size() > maximum_instruments)
        result.instruments.resize(maximum_instruments);
    if (!result.instruments.empty())
        result.next_sequence =
            result.instruments.back().sequence;
    else
        result.next_sequence =
            std::max(after_sequence, changes_.Newest());
    return result;
}

void MarketDataStore::Apply(
    const MarketDataEventPtr& owner,
    const QuoteReceived& event) {
    SymbolState& state =
        StateFor(event.quote.instrument_id, event.quote.symbol);
    if (state.quote && event.quote.event_time_ns > 0 &&
        state.quote->event_time_ns > 0 &&
        event.quote.event_time_ns < state.quote->event_time_ns)
        return;
    state.last_received_at_ms =
        std::max(state.last_received_at_ms,
                 event.quote.received_at_ms);
    state.quote_owner = owner;
    state.quote = &event.quote;
    ++state.revision;
    AppendEvent(state, owner);
    RecordChange(state);
}

void MarketDataStore::Apply(
    const MarketDataEventPtr& owner,
    const TradeReceived& event) {
    SymbolState& state =
        StateFor(event.trade.instrument_id, event.trade.symbol);
    const bool inserted =
        InsertTrade(state, owner, &event.trade);
    if (!inserted) return;
    state.last_received_at_ms =
        std::max(state.last_received_at_ms,
                 event.trade.received_at_ms);
    ++state.revision;
    AppendEvent(state, owner);
    RecordChange(state);
}

void MarketDataStore::Apply(
    const MarketDataEventPtr& owner,
    const TradeCanceled& event) {
    SymbolState& state =
        StateFor(event.instrument_id, event.symbol);
    if (EraseTrade(
            state, event.trade_id, event.event_time_ns))
        ++state.revision;
    AppendEvent(state, owner);
    RecordChange(state);
}

void MarketDataStore::Apply(
    const MarketDataEventPtr& owner,
    const TradeCorrected& event) {
    SymbolState& state =
        StateFor(event.instrument_id, event.symbol);
    EraseTrade(state, event.original_trade_id,
               event.corrected_trade.event_time_ns);
    InsertTrade(state, owner, &event.corrected_trade, true);
    state.last_received_at_ms =
        std::max(state.last_received_at_ms,
                 event.corrected_trade.received_at_ms);
    ++state.revision;
    AppendEvent(state, owner);
    RecordChange(state);
}

void MarketDataStore::Apply(
    const MarketDataEventPtr&,
    const MarketStreamChanged& event) {
    feed_ = event.feed;
    stream_status_ = event.status;
    status_message_ = event.message;
    trade_subscriptions_.clear();
    quote_subscriptions_.clear();
    trade_subscriptions_.insert(event.trade_symbols.begin(),
                                event.trade_symbols.end());
    quote_subscriptions_.insert(event.quote_symbols.begin(),
                                event.quote_symbols.end());
    for (auto& [symbol, state] : symbols_) {
        static_cast<void>(symbol);
        ++state->revision;
        RecordChange(*state);
    }
}

MarketDataStore::SymbolState& MarketDataStore::StateFor(
    std::string_view instrument_id, std::string_view symbol) {
    const std::string key =
        instrument_id.empty() ? std::string(symbol)
                              : std::string(instrument_id);
    if (!symbol.empty()) {
        const auto alias = keys_by_symbol_.find(std::string(symbol));
        if (alias != keys_by_symbol_.end() && alias->second != key) {
            const auto previous = symbols_.find(alias->second);
            if (previous != symbols_.end() &&
                !symbols_.contains(key)) {
                SymbolState* state = previous->second;
                symbols_.erase(previous);
                symbols_.emplace(key, state);
            }
        }
        keys_by_symbol_.insert_or_assign(std::string(symbol), key);
    }
    auto found = symbols_.find(key);
    if (found == symbols_.end()) {
        state_storage_.push_back(
            std::make_unique<SymbolState>(
                maximum_trades_per_symbol_));
        found = symbols_.emplace(
            key, state_storage_.back().get()).first;
    }
    SymbolState& state = *found->second;
    if (!instrument_id.empty())
        state.instrument_id = instrument_id;
    if (!symbol.empty()) state.symbol = symbol;
    return state;
}

const MarketDataStore::SymbolState* MarketDataStore::FindState(
    std::string_view identifier) const {
    std::string key(identifier);
    if (const auto alias = keys_by_symbol_.find(key);
        alias != keys_by_symbol_.end())
        key = alias->second;
    const auto found = symbols_.find(key);
    return found == symbols_.end() ? nullptr : found->second;
}

void MarketDataStore::RecordChange(SymbolState& state) {
    state.change_sequence = next_change_sequence_++;
    changes_.Push(state.change_sequence, &state);
}

bool MarketDataStore::InsertTrade(
    SymbolState& state, MarketDataEventPtr owner,
    const MarketTrade* trade, bool corrected) {
    if (!trade) return false;
    const std::string identity =
        TradeIdentity(trade->trade_id, trade->event_time_ns);
    if (!identity.empty() &&
        state.trade_slot_by_identity.contains(identity))
        return false;
    if (trade->event_time_ns <= 0 && !trade->trade_id.empty()) {
        const bool duplicate = std::ranges::any_of(
            state.trade_slots, [&](const TradeSlot& slot) {
                return slot.active && slot.trade &&
                       slot.trade->trade_id == trade->trade_id;
            });
        if (duplicate) return false;
    }

    TradeSlot& target =
        state.trade_slots[state.next_trade_slot];
    if (target.active && target.trade &&
        trade->event_time_ns > 0 &&
        target.trade->event_time_ns > trade->event_time_ns)
        return false;
    if (target.active) {
        if (!target.identity.empty()) {
            const auto indexed =
                state.trade_slot_by_identity.find(target.identity);
            if (indexed != state.trade_slot_by_identity.end() &&
                indexed->second == state.next_trade_slot)
                state.trade_slot_by_identity.erase(indexed);
        }
    } else {
        ++state.retained_trade_count;
    }
    target = {
        .owner = std::move(owner),
        .trade = trade,
        .identity = identity,
        .receive_sequence = next_receive_sequence_++,
        .corrected = corrected,
        .active = true,
    };
    if (!identity.empty())
        state.trade_slot_by_identity.insert_or_assign(
            identity, state.next_trade_slot);
    state.next_trade_slot =
        (state.next_trade_slot + 1) %
        state.trade_slots.size();
    return true;
}

bool MarketDataStore::EraseTrade(
    SymbolState& state, std::string_view trade_id,
    std::int64_t event_time_ns) {
    if (trade_id.empty()) return false;
    std::optional<std::size_t> slot_index;
    const std::string identity =
        TradeIdentity(trade_id, event_time_ns);
    if (event_time_ns > 0) {
        if (const auto found =
                state.trade_slot_by_identity.find(identity);
            found != state.trade_slot_by_identity.end())
            slot_index = found->second;
    }
    if (!slot_index) {
        for (std::size_t index = 0;
             index < state.trade_slots.size(); ++index) {
            const TradeSlot& slot = state.trade_slots[index];
            if (slot.active && slot.trade &&
                slot.trade->trade_id == trade_id) {
                slot_index = index;
                break;
            }
        }
    }
    if (!slot_index) return false;
    TradeSlot& slot = state.trade_slots[*slot_index];
    if (!slot.identity.empty()) {
        const auto indexed =
            state.trade_slot_by_identity.find(slot.identity);
        if (indexed != state.trade_slot_by_identity.end() &&
            indexed->second == *slot_index)
            state.trade_slot_by_identity.erase(indexed);
    }
    slot = {};
    --state.retained_trade_count;
    return true;
}

void MarketDataStore::AppendEvent(
    SymbolState& state, MarketDataEventPtr event) {
    const std::uint64_t sequence = state.next_sequence++;
    state.events.Push(sequence, std::move(event));
}

}  // namespace tradebox::core
