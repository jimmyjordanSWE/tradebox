#include "tradebox/core/market_data_store.h"

#include <algorithm>
#include <utility>

namespace tradebox::core {
namespace {

constexpr std::int64_t kDayNs =
    24LL * 60 * 60 * 1'000'000'000;
constexpr std::int64_t kMinuteNs = 60LL * 1'000'000'000;

std::int64_t ObservationTimeMs(
    std::int64_t event_time_ns,
    std::int64_t received_at_ms) {
    if (received_at_ms > 0) return received_at_ms;
    return event_time_ns > 0 ? event_time_ns / 1'000'000 : 0;
}

struct MinuteUpdateRules {
    bool price = false;
    bool volume = false;
};

MinuteUpdateRules RulesFor(const MarketTrade& trade) {
    const auto applies_to_tape =
        [&](std::string_view tapes) {
            return trade.tape.size() == 1 &&
                   tapes.contains(trade.tape.front());
        };
    if (trade.conditions.empty())
        return {.price = applies_to_tape("AB"),
                .volume = applies_to_tape("AB")};

    MinuteUpdateRules combined{
        .price = true,
        .volume = true,
    };
    for (const std::string& condition : trade.conditions) {
        MinuteUpdateRules rule;
        if (condition == "@") {
            rule = {.price = applies_to_tape("CO"),
                    .volume = applies_to_tape("CO")};
        } else if (condition == "A" || condition == "D") {
            rule = {.price = applies_to_tape("C"),
                    .volume = applies_to_tape("C")};
        } else if (condition == "B") {
            rule = {.price = applies_to_tape("C"),
                    .volume = applies_to_tape("ABC")};
        } else if (condition == "E") {
            rule = {.price = applies_to_tape("AB"),
                    .volume = applies_to_tape("AB")};
        } else if (condition == "F" || condition == "K" ||
                   condition == "L" || condition == "O" ||
                   condition == "X" || condition == "5" ||
                   condition == "6") {
            rule = {.price = applies_to_tape("ABC"),
                    .volume = applies_to_tape("ABC")};
        } else if (condition == "Y") {
            rule = {.price = applies_to_tape("C"),
                    .volume = applies_to_tape("C")};
        } else if (condition == "T") {
            rule = {.price = applies_to_tape("ABCO"),
                    .volume = applies_to_tape("ABCO")};
        } else if (condition == "C" || condition == "G" ||
                   condition == "H" || condition == "I" ||
                   condition == "N" || condition == "P" ||
                   condition == "R" || condition == "U" ||
                   condition == "V" || condition == "W" ||
                   condition == "Z" || condition == "4" ||
                   condition == "7") {
            rule = {.price = false,
                    .volume = applies_to_tape("ABCO")};
        } else {
            // M, Q, 9, and unknown conditions cannot safely
            // contribute to an Alpaca-compatible minute projection.
            rule = {};
        }
        combined.price = combined.price && rule.price;
        combined.volume = combined.volume && rule.volume;
    }
    return combined;
}

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

void MarketDataStore::IngestBatch(
    std::vector<MarketDataEventPtr> events) {
    if (events.empty()) return;
    std::scoped_lock lock(mutex_);
    for (MarketDataEventPtr& event : events) {
        if (!event) continue;
        std::visit(
            [this, &event](const auto& typed) {
                Apply(event, typed);
            },
            *event);
    }
}

MarketDataSnapshot MarketDataStore::Snapshot(
    const std::string& symbol) const {
    std::scoped_lock lock(mutex_);
    return SnapshotLocked(symbol);
}

MarketDataFrame MarketDataStore::SnapshotFrame(
    std::span<const std::string> identifiers) const {
    std::scoped_lock lock(mutex_);
    MarketDataFrame result{
        .publication_revision = next_change_sequence_ - 1,
    };
    result.instruments.reserve(identifiers.size());
    std::unordered_set<std::string> published;
    for (const std::string& identifier : identifiers) {
        if (identifier.empty()) continue;
        MarketDataSnapshot snapshot = SnapshotLocked(identifier);
        const std::string& key = snapshot.instrument_id.empty()
                                     ? snapshot.symbol
                                     : snapshot.instrument_id;
        if (!published.insert(key).second) continue;
        result.instruments.push_back(
            std::make_shared<const MarketDataSnapshot>(
                std::move(snapshot)));
    }
    return result;
}

MarketDataSnapshot MarketDataStore::SnapshotLocked(
    std::string_view symbol) const {
    const std::string identifier(symbol);
    MarketDataSnapshot result{
        .symbol = identifier,
        .feed = feed_,
        .stream_status = stream_status_,
        .trades_subscribed = trade_subscriptions_.contains(identifier),
        .quotes_subscribed = quote_subscriptions_.contains(identifier),
        .statuses_subscribed =
            status_subscriptions_.contains("*") ||
            status_subscriptions_.contains(identifier),
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
    result.statuses_subscribed =
        status_subscriptions_.contains("*") ||
        status_subscriptions_.contains(state->symbol);
    if (const auto started =
            trade_subscription_started_at_ns_.find(
                state->symbol);
        started !=
        trade_subscription_started_at_ns_.end())
        result.projection_started_at_ns = started->second;
    if (state->quote) result.latest_quote = *state->quote;
    result.latest_price = state->latest_price;
    if (state->trading_status)
        result.trading_status = *state->trading_status;
    for (const auto& minute : state->live_minutes) {
        const auto& bar = minute.bar;
        if (bar.start_ns == state->newest_minute_start_ns &&
            !bar.open.IsZero() && !bar.high.IsZero() &&
            !bar.low.IsZero() && !bar.close.IsZero() &&
            !bar.volume.IsZero())
            result.provisional_minute_bars.push_back(bar);
    }

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
    result.trade_pressure = state->trade_pressure.Snapshot(
        state->last_received_at_ms);
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
        .statuses_subscribed =
            status_subscriptions_.contains("*") ||
            status_subscriptions_.contains(symbol),
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
    result.statuses_subscribed =
        status_subscriptions_.contains("*") ||
        status_subscriptions_.contains(state.symbol);
    if (const auto started =
            trade_subscription_started_at_ns_.find(
                state.symbol);
        started !=
        trade_subscription_started_at_ns_.end())
        result.projection_started_at_ns = started->second;
    result.latest_price = state.latest_price;
    if (state.trading_status)
        result.trading_status = *state.trading_status;
    for (const auto& minute : state.live_minutes) {
        const auto& bar = minute.bar;
        if (bar.start_ns == state.newest_minute_start_ns &&
            !bar.open.IsZero() && !bar.high.IsZero() &&
            !bar.low.IsZero() && !bar.close.IsZero() &&
            !bar.volume.IsZero())
            result.provisional_minute_bars.push_back(bar);
    }
    result.next_sequence = state.next_sequence - 1;
    result.last_received_at_ms = state.last_received_at_ms;
    result.trade_pressure = state.trade_pressure.Snapshot(
        state.last_received_at_ms);
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
    state.trade_pressure.ObserveQuote(
        event.quote,
        ObservationTimeMs(event.quote.event_time_ns,
                          event.quote.received_at_ms));
    ++state.revision;
    AppendEvent(state, owner);
    RecordChange(state);
}

void MarketDataStore::Apply(
    const MarketDataEventPtr& owner,
    const TradeReceived& event) {
    SymbolState& state =
        StateFor(event.trade.instrument_id, event.trade.symbol);
    const std::uint64_t receive_sequence =
        next_receive_sequence_;
    const bool inserted =
        InsertTrade(state, owner, &event.trade);
    if (!inserted) return;
    state.trade_pressure.ObserveTrade(
        event.trade,
        ObservationTimeMs(event.trade.event_time_ns,
                          event.trade.received_at_ms));
    InsertLiveTrade(state, owner, event.trade,
                    receive_sequence);
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
    const bool erased = EraseTrade(
        state, event.trade_id, event.event_time_ns);
    const bool erased_live = EraseLiveTrade(
        state, event.trade_id, event.event_time_ns);
    const bool pressure_changed = state.trade_pressure.CancelTrade(
        event.trade_id, event.event_time_ns,
        ObservationTimeMs(event.event_time_ns,
                          event.received_at_ms));
    if (erased || erased_live || pressure_changed)
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
    EraseLiveTrade(state, event.original_trade_id,
                   event.corrected_trade.event_time_ns);
    state.trade_pressure.CorrectTrade(
        event.original_trade_id, event.corrected_trade,
        ObservationTimeMs(
            event.corrected_trade.event_time_ns,
            event.corrected_trade.received_at_ms));
    const std::uint64_t receive_sequence =
        next_receive_sequence_;
    InsertTrade(state, owner, &event.corrected_trade, true);
    InsertLiveTrade(state, owner, event.corrected_trade,
                    receive_sequence);
    state.last_received_at_ms =
        std::max(state.last_received_at_ms,
                 event.corrected_trade.received_at_ms);
    ++state.revision;
    AppendEvent(state, owner);
    RecordChange(state);
}

void MarketDataStore::Apply(
    const MarketDataEventPtr& owner,
    const TradingStatusReceived& event) {
    SymbolState& state =
        StateFor(event.status.instrument_id,
                 event.status.symbol);
    if (state.trading_status &&
        event.status.event_time_ns > 0 &&
        state.trading_status->event_time_ns > 0 &&
        event.status.event_time_ns <
            state.trading_status->event_time_ns)
        return;
    state.trading_status_owner = owner;
    state.trading_status = &event.status;
    state.last_received_at_ms =
        std::max(state.last_received_at_ms,
                 event.status.received_at_ms);
    ++state.revision;
    AppendEvent(state, owner);
    RecordChange(state);
}

void MarketDataStore::Apply(
    const MarketDataEventPtr&,
    const MarketStreamChanged& event) {
    const MarketDataFeed next_feed =
        event.feed == MarketDataFeed::Unknown
            ? feed_
            : event.feed;
    const bool feed_changed =
        feed_ != MarketDataFeed::Unknown &&
        next_feed != MarketDataFeed::Unknown &&
        feed_ != next_feed;
    const bool connection_boundary =
        event.status == MarketStreamStatus::Connecting;
    const bool pressure_boundary =
        feed_changed || connection_boundary ||
        event.status == MarketStreamStatus::Disconnected ||
        event.status == MarketStreamStatus::Stale ||
        event.status == MarketStreamStatus::Error;
    feed_ = next_feed;
    stream_status_ = event.status;
    status_message_ = event.message;
    const auto previous_trade_subscriptions =
        trade_subscriptions_;
    trade_subscriptions_.clear();
    quote_subscriptions_.clear();
    status_subscriptions_.clear();
    trade_subscriptions_.insert(event.trade_symbols.begin(),
                                event.trade_symbols.end());
    quote_subscriptions_.insert(event.quote_symbols.begin(),
                                event.quote_symbols.end());
    status_subscriptions_.insert(
        event.status_symbols.begin(),
        event.status_symbols.end());
    std::erase_if(
        trade_subscription_started_at_ns_,
        [&](const auto& entry) {
            return !trade_subscriptions_.contains(entry.first);
        });
    if (event.status == MarketStreamStatus::Subscribed) {
        const std::int64_t started_at_ns =
            event.received_at_ms > 0
                ? event.received_at_ms * 1'000'000
                : 0;
        for (const std::string& symbol :
             trade_subscriptions_) {
            if (!previous_trade_subscriptions.contains(symbol) ||
                feed_changed || connection_boundary)
                trade_subscription_started_at_ns_
                    .insert_or_assign(symbol, started_at_ns);
        }
    }
    for (auto& [symbol, state] : symbols_) {
        static_cast<void>(symbol);
        if (feed_changed || connection_boundary) {
            state->live_trades.clear();
            state->live_trade_by_id.clear();
            state->live_minutes.clear();
            state->latest_price.reset();
            state->newest_minute_start_ns = 0;
            ++state->live_revision;
        }
        if (pressure_boundary)
            state->trade_pressure.MarkStale();
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

void MarketDataStore::InsertLiveTrade(
    SymbolState& state, const MarketDataEventPtr& owner,
    const MarketTrade& trade,
    std::uint64_t receive_sequence) {
    if (trade.event_time_ns <= 0) return;
    const std::int64_t minute_start =
        (trade.event_time_ns / kMinuteNs) * kMinuteNs;
    if (minute_start > state.newest_minute_start_ns) {
        state.newest_minute_start_ns = minute_start;
        const std::int64_t oldest_retained =
            minute_start - kMinuteNs;
        std::erase_if(
            state.live_trades,
            [oldest_retained](const SymbolState::LiveTrade& item) {
                return !item.active || !item.trade ||
                       item.trade->event_time_ns < oldest_retained;
            });
        state.live_trade_by_id.clear();
        for (std::size_t index = 0;
             index < state.live_trades.size(); ++index) {
            const auto& item = state.live_trades[index];
            if (item.active && item.trade &&
                !item.trade->trade_id.empty())
                state.live_trade_by_id.emplace(
                    item.trade->trade_id, index);
        }
        std::erase_if(
            state.live_minutes,
            [oldest_retained](const SymbolState::LiveMinute& item) {
                return item.bar.start_ns < oldest_retained;
            });
    }
    if (state.newest_minute_start_ns != 0 &&
        minute_start < state.newest_minute_start_ns - kMinuteNs)
        return;

    if (!trade.trade_id.empty() &&
        state.live_trade_by_id.contains(trade.trade_id))
        return;
    const std::size_t index = state.live_trades.size();
    state.live_trades.push_back({
        .owner = owner,
        .trade = &trade,
        .receive_sequence = receive_sequence,
    });
    if (!trade.trade_id.empty())
        state.live_trade_by_id.emplace(trade.trade_id, index);
    ApplyLiveContribution(state, trade, receive_sequence);
    ++state.live_revision;
    for (auto& minute : state.live_minutes)
        minute.bar.revision = state.live_revision;
}

bool MarketDataStore::EraseLiveTrade(
    SymbolState& state, std::string_view trade_id,
    std::int64_t event_time_ns) {
    if (trade_id.empty()) return false;
    const auto indexed = state.live_trade_by_id.find(trade_id);
    if (indexed == state.live_trade_by_id.end())
        return false;
    SymbolState::LiveTrade& item =
        state.live_trades[indexed->second];
    if (!item.active || !item.trade) return false;
    const MarketTrade& trade = *item.trade;
    if (event_time_ns > 0 && trade.event_time_ns > 0 &&
        trade.event_time_ns / kDayNs !=
            event_time_ns / kDayNs)
        return false;

    const MinuteUpdateRules rules = RulesFor(trade);
    const std::int64_t minute_start =
        (trade.event_time_ns / kMinuteNs) * kMinuteNs;
    auto minute = std::ranges::find(
        state.live_minutes, minute_start,
        [](const SymbolState::LiveMinute& value) {
            return value.bar.start_ns;
        });
    const bool rebuild_minute =
        rules.price && minute != state.live_minutes.end() &&
        (trade.price == minute->bar.high ||
         trade.price == minute->bar.low ||
         (trade.event_time_ns == minute->open_time_ns &&
          item.receive_sequence == minute->open_sequence) ||
         (trade.event_time_ns == minute->close_time_ns &&
          item.receive_sequence == minute->close_sequence));
    const bool rebuild_latest =
        state.latest_price &&
        state.latest_price->trade_id == trade_id;

    item.active = false;
    state.live_trade_by_id.erase(indexed);
    if (rebuild_minute) {
        RebuildLiveMinute(state, minute_start);
    } else if (rules.volume &&
               minute != state.live_minutes.end()) {
        minute->bar.volume -= trade.size;
        if (minute->bar.trade_count > 0)
            --minute->bar.trade_count;
    }
    if (rebuild_latest) RebuildLatestPrice(state);
    ++state.live_revision;
    for (auto& value : state.live_minutes)
        value.bar.revision = state.live_revision;
    return true;
}

void MarketDataStore::ApplyLiveContribution(
    SymbolState& state, const MarketTrade& trade,
    std::uint64_t receive_sequence) {
    const MinuteUpdateRules rules = RulesFor(trade);
    if (rules.price) {
        const bool newer =
            !state.latest_price ||
            trade.event_time_ns >
                state.latest_price->event_time_ns ||
            (trade.event_time_ns ==
                 state.latest_price->event_time_ns &&
             receive_sequence >
                 state.latest_price->receive_sequence);
        if (newer)
            state.latest_price = CanonicalMarketPrice{
                .price = trade.price,
                .trade_id = trade.trade_id,
                .broker_timestamp = trade.broker_timestamp,
                .event_time_ns = trade.event_time_ns,
                .received_at_ms = trade.received_at_ms,
                .receive_sequence = receive_sequence,
            };
    }
    if (!rules.price && !rules.volume) return;

    const std::int64_t start =
        (trade.event_time_ns / kMinuteNs) * kMinuteNs;
    auto found = std::ranges::find(
        state.live_minutes, start,
        [](const SymbolState::LiveMinute& minute) {
            return minute.bar.start_ns;
        });
    if (found == state.live_minutes.end()) {
        state.live_minutes.push_back({
            .bar = {.start_ns = start},
        });
        found = std::prev(state.live_minutes.end());
    }
    if (rules.price) {
        const bool first_price = found->bar.open.IsZero();
        const bool earlier =
            first_price ||
            trade.event_time_ns < found->open_time_ns ||
            (trade.event_time_ns == found->open_time_ns &&
             receive_sequence < found->open_sequence);
        const bool later =
            first_price ||
            trade.event_time_ns > found->close_time_ns ||
            (trade.event_time_ns == found->close_time_ns &&
             receive_sequence > found->close_sequence);
        if (earlier) {
            found->bar.open = trade.price;
            found->open_time_ns = trade.event_time_ns;
            found->open_sequence = receive_sequence;
        }
        if (first_price || trade.price > found->bar.high)
            found->bar.high = trade.price;
        if (first_price || trade.price < found->bar.low)
            found->bar.low = trade.price;
        if (later) {
            found->bar.close = trade.price;
            found->close_time_ns = trade.event_time_ns;
            found->close_sequence = receive_sequence;
        }
    }
    if (rules.volume) {
        found->bar.volume += trade.size;
        ++found->bar.trade_count;
    }
}

void MarketDataStore::RebuildLiveMinute(
    SymbolState& state, std::int64_t minute_start_ns) {
    auto minute = std::ranges::find(
        state.live_minutes, minute_start_ns,
        [](const SymbolState::LiveMinute& value) {
            return value.bar.start_ns;
        });
    if (minute == state.live_minutes.end()) return;
    *minute = {
        .bar = {.start_ns = minute_start_ns},
    };
    for (const SymbolState::LiveTrade& item :
         state.live_trades) {
        if (!item.active || !item.trade) continue;
        const std::int64_t start =
            (item.trade->event_time_ns / kMinuteNs) * kMinuteNs;
        if (start == minute_start_ns)
            ApplyLiveContribution(
                state, *item.trade, item.receive_sequence);
    }
}

void MarketDataStore::RebuildLatestPrice(SymbolState& state) {
    state.latest_price.reset();
    for (const SymbolState::LiveTrade& item :
         state.live_trades) {
        if (!item.active || !item.trade) continue;
        const MarketTrade& trade = *item.trade;
        if (!RulesFor(trade).price) continue;
        const bool newer =
            !state.latest_price ||
            trade.event_time_ns >
                state.latest_price->event_time_ns ||
            (trade.event_time_ns ==
                 state.latest_price->event_time_ns &&
             item.receive_sequence >
                 state.latest_price->receive_sequence);
        if (newer)
            state.latest_price = CanonicalMarketPrice{
                .price = trade.price,
                .trade_id = trade.trade_id,
                .broker_timestamp = trade.broker_timestamp,
                .event_time_ns = trade.event_time_ns,
                .received_at_ms = trade.received_at_ms,
                .receive_sequence = item.receive_sequence,
            };
    }
}

void MarketDataStore::AppendEvent(
    SymbolState& state, MarketDataEventPtr event) {
    const std::uint64_t sequence = state.next_sequence++;
    state.events.Push(sequence, std::move(event));
}

}  // namespace tradebox::core
