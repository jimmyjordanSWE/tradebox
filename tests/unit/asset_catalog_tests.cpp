#include "tradebox/core/asset_catalog.h"

#include <gtest/gtest.h>

namespace tradebox::core {
namespace {

TradableAsset Equity(std::string id, std::string symbol) {
    return {
        .symbol = std::move(symbol),
        .name = "Equity",
        .active = true,
        .tradable = true,
        .instrument_id = std::move(id),
    };
}

TEST(AssetCatalog, AlphabeticalMatchesPromotePreviouslySelectedAssets) {
    const std::vector<TradableAsset> assets{
        Equity("alpaca:aaa", "AAA"),
        Equity("alpaca:aab", "AAB"),
        Equity("alpaca:aac", "AAC"),
    };
    const std::vector<std::string> history{"alpaca:aac"};

    const auto matches = SearchTradableAssets(assets, "AA", 3, history);

    ASSERT_EQ(matches.size(), 3U);
    EXPECT_EQ(matches[0].symbol, "AAC");
    EXPECT_EQ(matches[1].symbol, "AAA");
    EXPECT_EQ(matches[2].symbol, "AAB");
}

TEST(AssetCatalog, WithoutHistoryMatchesAreAlphabetical) {
    const std::vector<TradableAsset> assets{
        Equity("alpaca:aab", "AAB"),
        Equity("alpaca:aaa", "AAA"),
    };

    const auto matches = SearchTradableAssets(assets, "AA");

    ASSERT_EQ(matches.size(), 2U);
    EXPECT_EQ(matches[0].symbol, "AAA");
    EXPECT_EQ(matches[1].symbol, "AAB");
}

}  // namespace
}  // namespace tradebox::core
