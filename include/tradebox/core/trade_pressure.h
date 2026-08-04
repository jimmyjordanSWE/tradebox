#pragma once

#include "tradebox/core/market_data.h"
#include "tradebox/core/trade_pressure_types.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace tradebox::core {

class TradePressureReducer final {
public:
    explicit TradePressureReducer(TradePressureConfig config = {});

    // observation_time_ms is supplied by the caller; this class never reads
    // a system clock. The return value is false for an already-seen trade.
    bool ObserveQuote(const MarketQuote& quote,
                      std::int64_t observation_time_ms);
    bool ObserveTrade(const MarketTrade& trade,
                      std::int64_t observation_time_ms);
    bool CorrectTrade(std::string_view original_trade_id,
                      const MarketTrade& corrected_trade,
                      std::int64_t observation_time_ms);
    bool CancelTrade(std::string_view trade_id,
                     std::int64_t event_time_ns,
                     std::int64_t observation_time_ms);

    // A disconnect or feed change starts a new transient feature window.
    void MarkStale();

    [[nodiscard]] TradePressureSnapshot Snapshot(
        std::int64_t now_ms) const;
    [[nodiscard]] std::size_t RecentContributionCount() const noexcept;

private:
    struct QuoteContext {
        MarketQuote quote;
        std::int64_t source_time_ms = 0;
    };

    struct Contribution {
        std::string identity;
        std::string trade_id;
        std::int64_t event_time_ns = 0;
        std::int64_t applied_at_ms = 0;
        Decimal price;
        TradeInitiation initiation = TradeInitiation::Unknown;
        TradeClassificationMethod method =
            TradeClassificationMethod::Unknown;
        double weight = 0.0;
    };

    struct Classification {
        TradeInitiation initiation = TradeInitiation::Unknown;
        TradeClassificationMethod method =
            TradeClassificationMethod::Unknown;
    };

    [[nodiscard]] std::int64_t EffectiveObservationTime(
        std::int64_t observation_time_ms) const noexcept;
    [[nodiscard]] std::int64_t TradeTime(
        const MarketTrade& trade,
        std::int64_t fallback_ms) const noexcept;
    [[nodiscard]] std::string Identity(
        std::string_view trade_id,
        std::int64_t event_time_ns) const;
    [[nodiscard]] Classification Classify(
        const MarketTrade& trade,
        std::int64_t trade_time_ms) const;
    [[nodiscard]] TradeInitiation TickDirection(
        const MarketTrade& trade,
        std::int64_t trade_time_ms) const;
    [[nodiscard]] double WeightFor(
        const MarketTrade& trade) const noexcept;
    void DecayTo(std::int64_t now_ms);
    void Prune(std::int64_t now_ms);
    void Rebuild(std::int64_t now_ms);
    void RemoveContributionWeight(const Contribution& contribution,
                                  std::int64_t now_ms);
    void RememberTick(const MarketTrade& trade,
                      TradeInitiation direction,
                      std::int64_t now_ms);
    [[nodiscard]] std::optional<std::size_t> FindContribution(
        std::string_view trade_id,
        std::int64_t event_time_ns) const;

    TradePressureConfig config_;
    std::optional<QuoteContext> quote_;
    std::optional<Decimal> previous_distinct_trade_price_;
    TradeInitiation last_tick_direction_ = TradeInitiation::Unknown;
    std::int64_t last_tick_direction_ms_ = 0;
    TradeClassificationMethod latest_method_ =
        TradeClassificationMethod::Unknown;
    std::int64_t last_observation_ms_ = 0;
    bool stale_ = true;
    double buyer_weight_ = 0.0;
    double seller_weight_ = 0.0;
    double unknown_weight_ = 0.0;
    std::deque<Contribution> contributions_;
    std::unordered_set<std::string> contribution_identities_;
};

}  // namespace tradebox::core
