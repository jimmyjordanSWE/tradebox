#pragma once

#include "tradebox/core/market_data.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tradebox::application {

enum class MarketDataInterestPriority : std::uint8_t {
    Background,
    UserVisible,
    Strategy,
    TradingCritical,
};

struct MarketDataChannels {
    bool trades = true;
    bool quotes = true;
    bool statuses = true;

    bool operator==(const MarketDataChannels&) const = default;
};

struct MarketDataInterest {
    std::string consumer_id;
    core::MarketDataFeed feed = core::MarketDataFeed::Iex;
    std::vector<std::string> symbols;
    MarketDataChannels channels;
    MarketDataInterestPriority priority =
        MarketDataInterestPriority::UserVisible;

    bool operator==(const MarketDataInterest&) const = default;
};

struct EffectiveMarketDataSubscription {
    std::string symbol;
    MarketDataChannels channels;
    MarketDataInterestPriority priority =
        MarketDataInterestPriority::Background;
    std::vector<std::string> consumers;
};

struct RejectedMarketDataSubscription {
    std::string symbol;
    MarketDataInterestPriority priority =
        MarketDataInterestPriority::Background;
    std::vector<std::string> consumers;
    std::string reason;
};

struct MarketDataSubscriptionPlan {
    std::uint64_t revision = 0;
    core::MarketDataFeed feed = core::MarketDataFeed::Unknown;
    std::size_t capacity = 0;
    std::vector<EffectiveMarketDataSubscription> accepted;
    std::vector<RejectedMarketDataSubscription> rejected;

    [[nodiscard]] std::vector<std::string> Symbols() const;
};

// The sole application authority that combines market-data requirements from
// trading-critical services, strategies, visible clients, and background
// consumers into one deterministic feed-specific admission plan.
class MarketDataInterestCoordinator final {
public:
    explicit MarketDataInterestCoordinator(
        std::size_t symbol_capacity = 30);

    [[nodiscard]] std::expected<MarketDataSubscriptionPlan, std::string>
    Upsert(MarketDataInterest interest);
    [[nodiscard]] bool Remove(std::string_view consumer_id);
    [[nodiscard]] MarketDataSubscriptionPlan Plan(
        core::MarketDataFeed feed) const;

private:
    [[nodiscard]] MarketDataSubscriptionPlan PlanLocked(
        core::MarketDataFeed feed) const;

    const std::size_t symbol_capacity_;
    mutable std::mutex mutex_;
    std::vector<MarketDataInterest> interests_;
    std::uint64_t revision_ = 0;
};

}  // namespace tradebox::application
