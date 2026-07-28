#include "tradebox/core/asset_catalog.h"

#include <algorithm>
#include <cctype>

namespace tradebox::core {

std::vector<TradableAsset> SearchTradableAssets(
    const std::vector<TradableAsset>& assets, std::string query,
    std::size_t limit) {
    std::ranges::transform(query, query.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    struct Match { const TradableAsset* asset; int group; };
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
        matches.push_back({&asset, symbol == query ? 0 : symbol_prefix ? 1 : 2});
    }
    std::ranges::stable_sort(matches, [](const Match& left, const Match& right) {
        if (left.group != right.group) return left.group < right.group;
        if (left.asset->previous_dollar_volume != right.asset->previous_dollar_volume)
            return left.asset->previous_dollar_volume > right.asset->previous_dollar_volume;
        if (left.asset->previous_volume != right.asset->previous_volume)
            return left.asset->previous_volume > right.asset->previous_volume;
        return left.asset->symbol < right.asset->symbol;
    });
    std::vector<TradableAsset> result;
    for (const Match& match : matches) {
        if (result.size() == limit) break;
        result.push_back(*match.asset);
    }
    return result;
}

}  // namespace tradebox::core
