#include "tradebox/ui/model.h"

#include <gtest/gtest.h>

TEST(UiOrderEntry, ValidatesRequiredPricesAndAmount) {
    OrderEntryDraft draft;
    draft.amount.clear();
    draft.type = "Stop-limit";
    const auto errors = ValidateOrderEntry(draft);
    EXPECT_EQ(errors.size(), 3U);
}

TEST(UiOrderEntry, RejectsUnsupportedNotionalAndExtendedHours) {
    OrderEntryDraft draft;
    draft.amount_is_notional = true;
    draft.type = "Limit";
    draft.extended_hours = true;
    draft.time_in_force = "Gtc";
    EXPECT_EQ(ValidateOrderEntry(draft).size(), 3U);
}

TEST(UiOrderEntry, LabelsEveryDeterministicState) {
    EXPECT_EQ(UiOrderStateLabel(UiOrderState::Pending), "pending");
    EXPECT_EQ(UiOrderStateLabel(UiOrderState::Indeterminate), "indeterminate");
    EXPECT_EQ(UiOrderStateLabel(UiOrderState::Reconciling), "reconciling");
}

TEST(UiOrderEntry, MapsCoreOrderStates) {
    tradebox::core::OrderState order;
    tradebox::core::CoreSnapshot snapshot;
    order.status = "filled";
    EXPECT_EQ(UiOrderStateFromCore(order, snapshot), UiOrderState::Filled);
    order.status = "canceled";
    EXPECT_EQ(UiOrderStateFromCore(order, snapshot), UiOrderState::Canceled);
    snapshot.safety_status = tradebox::core::SafetyStatus::Stale;
    EXPECT_EQ(UiOrderStateFromCore(order, snapshot), UiOrderState::Stale);
}

TEST(AssetCatalog, FiltersAndRanksPrefixMatchesByActivity) {
    const std::vector<tradebox::core::TradableAsset> assets = {
        {"AMD", "Advanced Micro Devices", "NASDAQ", true, true, false, true, 1, 100},
        {"AMAT", "Applied Materials", "NASDAQ", true, true, false, true, 2, 200},
        {"AMTX", "Aemetis", "NASDAQ", true, false, false, true, 999, 999},
        {"CGRNQ", "OTC example", "OTC", true, true, false, true, 999, 999},
    };
    const auto matches = tradebox::core::SearchTradableAssets(assets, "AM", 5);
    ASSERT_EQ(matches.size(), 2U);
    EXPECT_EQ(matches[0].symbol, "AMAT");
    EXPECT_EQ(matches[1].symbol, "AMD");
}

TEST(AssetCatalog, ExactSymbolPrecedesHigherVolumePrefix) {
    const std::vector<tradebox::core::TradableAsset> assets = {
        {"A", "A", "NYSE", true, true, false, false, 1, 1},
        {"AA", "AA", "NYSE", true, true, false, false, 100, 100},
    };
    const auto matches = tradebox::core::SearchTradableAssets(assets, "A", 5);
    ASSERT_EQ(matches.size(), 2U);
    EXPECT_EQ(matches[0].symbol, "A");
}
