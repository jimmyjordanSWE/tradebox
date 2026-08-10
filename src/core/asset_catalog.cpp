#include "tradebox/core/asset_catalog.h"

#include <algorithm>
#include <cctype>

namespace tradebox::core {

std::vector<TradableAsset> SearchTradableAssets(
    const std::vector<TradableAsset>& assets, std::string query,
    std::size_t limit,
    std::span<const std::string> preferred_instrument_ids) {
    std::ranges::transform(query, query.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    struct Match {
        const TradableAsset* asset;
        int group;
        std::size_t preference_rank;
    };
    std::vector<Match> matches;
    for (const auto& asset : assets) {
        if (!asset.active || !asset.tradable || asset.exchange == "OTC") continue;
        std::string symbol = asset.symbol;
        std::ranges::transform(symbol, symbol.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        std::string name = asset.name;
        std::ranges::transform(name, name.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        const bool symbol_prefix = symbol.starts_with(query);
        const bool name_match = !query.empty() && name.find(query) != std::string::npos;
        if (!query.empty() && !symbol_prefix && !name_match) continue;
        const auto preferred = std::ranges::find(
            preferred_instrument_ids, asset.instrument_id);
        matches.push_back({
            &asset,
            symbol == query ? 0 : symbol_prefix ? 1 : 2,
            preferred == preferred_instrument_ids.end()
                ? preferred_instrument_ids.size()
                : static_cast<std::size_t>(
                      std::distance(preferred_instrument_ids.begin(), preferred)),
        });
    }
    std::ranges::stable_sort(matches, [](const Match& left, const Match& right) {
        if (left.group != right.group) return left.group < right.group;
        if (left.preference_rank != right.preference_rank)
            return left.preference_rank < right.preference_rank;
        return left.asset->symbol < right.asset->symbol;
    });
    if (!matches.empty() && matches.front().group == 0)
        return {*matches.front().asset};
    std::vector<TradableAsset> result;
    for (const Match& match : matches) {
        if (result.size() == limit) break;
        result.push_back(*match.asset);
    }
    return result;
}

}  // namespace tradebox::core
