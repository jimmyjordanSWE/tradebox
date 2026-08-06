#include "tradebox/application/market_data_interest.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <ranges>

namespace tradebox::application {
namespace {

void MergeChannels(MarketDataChannels& destination,
                   const MarketDataChannels& source) {
    destination.trades = destination.trades || source.trades;
    destination.quotes = destination.quotes || source.quotes;
    destination.statuses = destination.statuses || source.statuses;
}

}  // namespace

std::vector<std::string> MarketDataSubscriptionPlan::Symbols() const {
    std::vector<std::string> result;
    result.reserve(accepted.size());
    for (const auto& subscription : accepted)
        result.push_back(subscription.symbol);
    return result;
}

MarketDataInterestCoordinator::MarketDataInterestCoordinator(
    std::size_t symbol_capacity)
    : symbol_capacity_(symbol_capacity) {}

std::expected<MarketDataSubscriptionPlan, std::string>
MarketDataInterestCoordinator::Upsert(MarketDataInterest interest) {
    if (interest.consumer_id.empty())
        return std::unexpected("Market-data interest requires a consumer ID");
    if (interest.feed == core::MarketDataFeed::Unknown)
        return std::unexpected("Market-data interest requires a concrete feed");
    std::ranges::sort(interest.symbols);
    std::erase_if(interest.symbols,
                  [](const std::string& symbol) { return symbol.empty(); });
    const auto unique = std::ranges::unique(interest.symbols);
    interest.symbols.erase(unique.begin(), unique.end());
    const core::MarketDataFeed feed = interest.feed;

    std::scoped_lock lock(mutex_);
    const auto found = std::ranges::find(
        interests_, interest.consumer_id, &MarketDataInterest::consumer_id);
    if (found != interests_.end() && *found == interest)
        return PlanLocked(interest.feed);
    if (found == interests_.end())
        interests_.push_back(std::move(interest));
    else
        *found = std::move(interest);
    ++revision_;
    return PlanLocked(feed);
}

bool MarketDataInterestCoordinator::Remove(std::string_view consumer_id) {
    std::scoped_lock lock(mutex_);
    const auto previous_size = interests_.size();
    std::erase_if(interests_, [&](const MarketDataInterest& interest) {
        return interest.consumer_id == consumer_id;
    });
    if (interests_.size() == previous_size) return false;
    ++revision_;
    return true;
}

MarketDataSubscriptionPlan MarketDataInterestCoordinator::Plan(
    core::MarketDataFeed feed) const {
    std::scoped_lock lock(mutex_);
    return PlanLocked(feed);
}

MarketDataSubscriptionPlan MarketDataInterestCoordinator::PlanLocked(
    core::MarketDataFeed feed) const {
    std::map<std::string, EffectiveMarketDataSubscription, std::less<>> merged;
    for (const MarketDataInterest& interest : interests_) {
        if (interest.feed != feed) continue;
        for (const std::string& symbol : interest.symbols) {
            auto [found, inserted] = merged.try_emplace(
                symbol,
                EffectiveMarketDataSubscription{
                    .symbol = symbol,
                    .channels = interest.channels,
                    .priority = interest.priority,
                });
            if (!inserted) {
                MergeChannels(found->second.channels, interest.channels);
                found->second.priority = std::max(
                    found->second.priority, interest.priority);
            }
            found->second.consumers.push_back(interest.consumer_id);
        }
    }

    std::vector<EffectiveMarketDataSubscription> candidates;
    candidates.reserve(merged.size());
    for (auto& [symbol, subscription] : merged) {
        static_cast<void>(symbol);
        std::ranges::sort(subscription.consumers);
        candidates.push_back(std::move(subscription));
    }
    std::ranges::sort(
        candidates, [](const auto& left, const auto& right) {
            if (left.priority != right.priority)
                return left.priority > right.priority;
            return left.symbol < right.symbol;
        });

    MarketDataSubscriptionPlan result{
        .revision = revision_,
        .feed = feed,
        .capacity = symbol_capacity_,
    };
    const std::size_t admitted = std::min(
        symbol_capacity_, candidates.size());
    result.accepted.insert(
        result.accepted.end(),
        std::make_move_iterator(candidates.begin()),
        std::make_move_iterator(candidates.begin() +
                                static_cast<std::ptrdiff_t>(admitted)));
    for (std::size_t index = admitted; index < candidates.size(); ++index) {
        result.rejected.push_back({
            .symbol = std::move(candidates[index].symbol),
            .priority = candidates[index].priority,
            .consumers = std::move(candidates[index].consumers),
            .reason = "Market-data subscription capacity reached",
        });
    }
    std::ranges::sort(result.accepted, {},
                      &EffectiveMarketDataSubscription::symbol);
    std::ranges::sort(result.rejected, {},
                      &RejectedMarketDataSubscription::symbol);
    return result;
}

}  // namespace tradebox::application
