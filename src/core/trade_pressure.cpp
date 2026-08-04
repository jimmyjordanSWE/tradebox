#include "tradebox/core/trade_pressure.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace tradebox::core {
namespace {

constexpr std::int64_t kDayNs =
    24LL * 60 * 60 * 1'000'000'000;
constexpr std::int64_t kNsPerMs = 1'000'000;

double DecayFactor(std::int64_t elapsed_ms,
                   std::int64_t half_life_ms) {
    if (elapsed_ms <= 0) return 1.0;
    return std::exp2(-static_cast<double>(elapsed_ms) /
                     static_cast<double>(half_life_ms));
}

}  // namespace

TradePressureReducer::TradePressureReducer(
    TradePressureConfig config)
    : config_(std::move(config)) {
    config_.half_life_ms = std::max<std::int64_t>(
        config_.half_life_ms, 1);
    config_.quote_max_age_ms = std::max<std::int64_t>(
        config_.quote_max_age_ms, 0);
    config_.tick_direction_max_age_ms = std::max<std::int64_t>(
        config_.tick_direction_max_age_ms, 0);
    config_.correction_horizon_ms = std::max<std::int64_t>(
        config_.correction_horizon_ms, 0);
    config_.maximum_recent_contributions = std::max<std::size_t>(
        config_.maximum_recent_contributions, 1);
    config_.activity_saturation_weight = std::max(
        config_.activity_saturation_weight, 0.0);
}

bool TradePressureReducer::ObserveQuote(
    const MarketQuote& quote, std::int64_t observation_time_ms) {
    const std::int64_t now =
        EffectiveObservationTime(observation_time_ms);
    const std::int64_t source_time =
        quote.event_time_ns > 0
            ? quote.event_time_ns / kNsPerMs
            : quote.received_at_ms > 0
                  ? quote.received_at_ms
                  : now;
    if (quote_ && source_time > 0 &&
        quote_->source_time_ms > 0 &&
        source_time < quote_->source_time_ms)
        return false;

    DecayTo(now);
    quote_ = QuoteContext{.quote = quote, .source_time_ms = source_time};
    last_observation_ms_ = std::max(last_observation_ms_, now);
    stale_ = false;
    return true;
}

bool TradePressureReducer::ObserveTrade(
    const MarketTrade& trade, std::int64_t observation_time_ms) {
    const std::string identity =
        Identity(trade.trade_id, trade.event_time_ns);
    if (!identity.empty() &&
        contribution_identities_.contains(identity))
        return false;

    const std::int64_t now =
        EffectiveObservationTime(observation_time_ms);
    DecayTo(now);
    Prune(now);
    const std::int64_t trade_time = TradeTime(trade, now);
    const Classification classification =
        Classify(trade, trade_time);
    const double weight = WeightFor(trade);
    contributions_.push_back({
        .identity = identity,
        .trade_id = trade.trade_id,
        .event_time_ns = trade.event_time_ns,
        .applied_at_ms = now,
        .price = trade.price,
        .initiation = classification.initiation,
        .method = classification.method,
        .weight = weight,
    });
    if (!identity.empty())
        contribution_identities_.insert(identity);
    if (classification.initiation == TradeInitiation::BuyerInitiated)
        buyer_weight_ += weight;
    else if (classification.initiation == TradeInitiation::SellerInitiated)
        seller_weight_ += weight;
    else
        unknown_weight_ += weight;
    if (classification.method != TradeClassificationMethod::Unknown)
        latest_method_ = classification.method;
    else
        latest_method_ = TradeClassificationMethod::Unknown;
    RememberTick(trade, classification.initiation, now);
    last_observation_ms_ = now;
    stale_ = false;

    while (contributions_.size() >
           config_.maximum_recent_contributions) {
        RemoveContributionWeight(contributions_.front(), now);
        if (!contributions_.front().identity.empty())
            contribution_identities_.erase(
                contributions_.front().identity);
        contributions_.pop_front();
    }
    return true;
}

bool TradePressureReducer::CorrectTrade(
    std::string_view original_trade_id,
    const MarketTrade& corrected_trade,
    std::int64_t observation_time_ms) {
    const std::int64_t now =
        EffectiveObservationTime(observation_time_ms);
    DecayTo(now);
    Prune(now);
    const auto found = FindContribution(
        original_trade_id, corrected_trade.event_time_ns);
    if (!found) return false;

    if (!contributions_[*found].identity.empty())
        contribution_identities_.erase(
            contributions_[*found].identity);
    contributions_.erase(contributions_.begin() +
                          static_cast<std::ptrdiff_t>(*found));
    Rebuild(now);
    return ObserveTrade(corrected_trade, now);
}

bool TradePressureReducer::CancelTrade(
    std::string_view trade_id, std::int64_t event_time_ns,
    std::int64_t observation_time_ms) {
    const std::int64_t now =
        EffectiveObservationTime(observation_time_ms);
    DecayTo(now);
    Prune(now);
    const auto found = FindContribution(trade_id, event_time_ns);
    if (!found) return false;
    if (!contributions_[*found].identity.empty())
        contribution_identities_.erase(
            contributions_[*found].identity);
    contributions_.erase(contributions_.begin() +
                          static_cast<std::ptrdiff_t>(*found));
    Rebuild(now);
    return true;
}

void TradePressureReducer::MarkStale() {
    quote_.reset();
    previous_distinct_trade_price_.reset();
    last_tick_direction_ = TradeInitiation::Unknown;
    last_tick_direction_ms_ = 0;
    latest_method_ = TradeClassificationMethod::Unknown;
    last_observation_ms_ = 0;
    stale_ = true;
    buyer_weight_ = 0.0;
    seller_weight_ = 0.0;
    unknown_weight_ = 0.0;
    contributions_.clear();
    contribution_identities_.clear();
}

TradePressureSnapshot TradePressureReducer::Snapshot(
    std::int64_t now_ms) const {
    const std::int64_t now =
        last_observation_ms_ > now_ms ? last_observation_ms_ : now_ms;
    const double buyer = buyer_weight_ *
                         DecayFactor(now - last_observation_ms_,
                                     config_.half_life_ms);
    const double seller = seller_weight_ *
                          DecayFactor(now - last_observation_ms_,
                                      config_.half_life_ms);
    const double unknown = unknown_weight_ *
                           DecayFactor(now - last_observation_ms_,
                                       config_.half_life_ms);
    const double gross = buyer + seller;
    const double pressure = gross > 0.0
                                ? (buyer - seller) / gross
                                : 0.0;
    const double saturation =
        config_.activity_saturation_weight + gross;
    const double activity =
        saturation > 0.0 ? gross / saturation : 0.0;
    TradePressureSnapshot result{
        .signed_pressure = std::clamp(pressure, -1.0, 1.0),
        .activity = std::clamp(activity, 0.0, 1.0),
        .buyer_weight = buyer,
        .seller_weight = seller,
        .unknown_weight = unknown,
        .latest_method = latest_method_,
        .last_observation_ms = last_observation_ms_,
        .half_life_ms = config_.half_life_ms,
        .stale = stale_ || last_observation_ms_ == 0,
    };
    for (const Contribution& contribution : contributions_) {
        if (contribution.initiation == TradeInitiation::BuyerInitiated)
            ++result.buyer_trades;
        else if (contribution.initiation == TradeInitiation::SellerInitiated)
            ++result.seller_trades;
        else
            ++result.unknown_trades;
    }
    return result;
}

std::size_t TradePressureReducer::RecentContributionCount() const noexcept {
    return contributions_.size();
}

std::int64_t TradePressureReducer::EffectiveObservationTime(
    std::int64_t observation_time_ms) const noexcept {
    return std::max(observation_time_ms, last_observation_ms_);
}

std::int64_t TradePressureReducer::TradeTime(
    const MarketTrade& trade, std::int64_t fallback_ms) const noexcept {
    if (trade.event_time_ns > 0)
        return trade.event_time_ns / kNsPerMs;
    if (trade.received_at_ms > 0) return trade.received_at_ms;
    return fallback_ms;
}

std::string TradePressureReducer::Identity(
    std::string_view trade_id, std::int64_t event_time_ns) const {
    if (trade_id.empty()) return {};
    if (event_time_ns <= 0)
        return std::string(trade_id) + "@?";
    return std::string(trade_id) + "@" +
           std::to_string(event_time_ns / kDayNs);
}

TradePressureReducer::Classification TradePressureReducer::Classify(
    const MarketTrade& trade, std::int64_t trade_time_ms) const {
    if (quote_) {
        const MarketQuote& quote = quote_->quote;
        const bool usable_prices =
            !quote.bid_price.IsZero() && !quote.ask_price.IsZero();
        const bool non_crossed =
            usable_prices && quote.bid_price < quote.ask_price;
        const std::int64_t age =
            trade_time_ms - quote_->source_time_ms;
        const bool fresh = age >= 0 &&
                           age <= config_.quote_max_age_ms;
        if (non_crossed && fresh) {
            if (trade.price >= quote.ask_price)
                return {.initiation = TradeInitiation::BuyerInitiated,
                        .method = TradeClassificationMethod::QuoteTest};
            if (trade.price <= quote.bid_price)
                return {.initiation = TradeInitiation::SellerInitiated,
                        .method = TradeClassificationMethod::QuoteTest};
        }
    }

    const TradeInitiation tick = TickDirection(trade, trade_time_ms);
    return {
        .initiation = tick,
        .method = tick == TradeInitiation::Unknown
                      ? TradeClassificationMethod::Unknown
                      : TradeClassificationMethod::TickTest,
    };
}

TradeInitiation TradePressureReducer::TickDirection(
    const MarketTrade& trade, std::int64_t trade_time_ms) const {
    if (!previous_distinct_trade_price_) return TradeInitiation::Unknown;
    if (trade.price > *previous_distinct_trade_price_)
        return TradeInitiation::BuyerInitiated;
    if (trade.price < *previous_distinct_trade_price_)
        return TradeInitiation::SellerInitiated;
    if (last_tick_direction_ != TradeInitiation::Unknown &&
        trade_time_ms >= last_tick_direction_ms_ &&
        trade_time_ms - last_tick_direction_ms_ <=
            config_.tick_direction_max_age_ms)
        return last_tick_direction_;
    return TradeInitiation::Unknown;
}

double TradePressureReducer::WeightFor(
    const MarketTrade& trade) const noexcept {
    switch (config_.weight) {
    case TradePressureWeight::Trades:
        return 1.0;
    case TradePressureWeight::Shares:
        return std::max(0.0, trade.size.ToDisplayDouble());
    case TradePressureWeight::Notional:
        return std::max(0.0, (trade.price * trade.size).ToDisplayDouble());
    }
    return 0.0;
}

void TradePressureReducer::DecayTo(std::int64_t now_ms) {
    const std::int64_t now = EffectiveObservationTime(now_ms);
    if (last_observation_ms_ == 0) {
        last_observation_ms_ = now;
        return;
    }
    const double factor = DecayFactor(
        now - last_observation_ms_, config_.half_life_ms);
    buyer_weight_ *= factor;
    seller_weight_ *= factor;
    unknown_weight_ *= factor;
    last_observation_ms_ = now;
}

void TradePressureReducer::Prune(std::int64_t now_ms) {
    while (!contributions_.empty() &&
           now_ms - contributions_.front().applied_at_ms >
               config_.correction_horizon_ms) {
        RemoveContributionWeight(contributions_.front(), now_ms);
        if (!contributions_.front().identity.empty())
            contribution_identities_.erase(
                contributions_.front().identity);
        contributions_.pop_front();
    }
}

void TradePressureReducer::Rebuild(std::int64_t now_ms) {
    Prune(now_ms);
    buyer_weight_ = 0.0;
    seller_weight_ = 0.0;
    unknown_weight_ = 0.0;
    latest_method_ = TradeClassificationMethod::Unknown;
    previous_distinct_trade_price_.reset();
    last_tick_direction_ = TradeInitiation::Unknown;
    last_tick_direction_ms_ = 0;
    for (const Contribution& contribution : contributions_) {
        const double weight = contribution.weight *
                              DecayFactor(
                                  now_ms - contribution.applied_at_ms,
                                  config_.half_life_ms);
        if (contribution.initiation == TradeInitiation::BuyerInitiated)
            buyer_weight_ += weight;
        else if (contribution.initiation == TradeInitiation::SellerInitiated)
            seller_weight_ += weight;
        else
            unknown_weight_ += weight;
        latest_method_ = contribution.method;
        RememberTick(
            MarketTrade{.price = contribution.price},
            contribution.initiation, contribution.applied_at_ms);
    }
    last_observation_ms_ = std::max(last_observation_ms_, now_ms);
}

void TradePressureReducer::RemoveContributionWeight(
    const Contribution& contribution, std::int64_t now_ms) {
    const double weight = contribution.weight *
                          DecayFactor(
                              now_ms - contribution.applied_at_ms,
                              config_.half_life_ms);
    if (contribution.initiation == TradeInitiation::BuyerInitiated)
        buyer_weight_ = std::max(0.0, buyer_weight_ - weight);
    else if (contribution.initiation == TradeInitiation::SellerInitiated)
        seller_weight_ = std::max(0.0, seller_weight_ - weight);
    else
        unknown_weight_ = std::max(0.0, unknown_weight_ - weight);
}

void TradePressureReducer::RememberTick(
    const MarketTrade& trade, TradeInitiation direction,
    std::int64_t now_ms) {
    if (previous_distinct_trade_price_ &&
        trade.price == *previous_distinct_trade_price_)
        return;
    previous_distinct_trade_price_ = trade.price;
    if (direction != TradeInitiation::Unknown) {
        last_tick_direction_ = direction;
        last_tick_direction_ms_ = now_ms;
    }
}

std::optional<std::size_t> TradePressureReducer::FindContribution(
    std::string_view trade_id, std::int64_t event_time_ns) const {
    if (trade_id.empty()) return std::nullopt;
    const std::string identity = Identity(trade_id, event_time_ns);
    if (event_time_ns > 0) {
        for (std::size_t index = 0; index < contributions_.size(); ++index)
            if (contributions_[index].identity == identity)
                return index;
    }
    for (std::size_t index = 0; index < contributions_.size(); ++index)
        if (contributions_[index].trade_id == trade_id)
            return index;
    return std::nullopt;
}

}  // namespace tradebox::core
