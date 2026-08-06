#include "watch_list_autocomplete.h"

#include <gtest/gtest.h>

namespace {

TEST(WatchListAutocompleteTest, FirstMatchIsSelectedByDefault) {
    EXPECT_EQ(tradebox::gui::SelectedWatchListAutocompleteMatch(-1, 3U), 0);
}

TEST(WatchListAutocompleteTest, DownMovesFromFirstToSecondMatch) {
    EXPECT_EQ(
        tradebox::gui::MoveWatchListAutocompleteMatch(0, 3U, 1), 1);
}

TEST(WatchListAutocompleteTest, NavigationStaysWithinVisibleMatches) {
    EXPECT_EQ(
        tradebox::gui::MoveWatchListAutocompleteMatch(-1, 3U, -1), 0);
    EXPECT_EQ(
        tradebox::gui::MoveWatchListAutocompleteMatch(2, 3U, 1), 2);
    EXPECT_EQ(
        tradebox::gui::MoveWatchListAutocompleteMatch(0, 3U, -1), 0);
    EXPECT_EQ(
        tradebox::gui::MoveWatchListAutocompleteMatch(0, 0U, 1), -1);
}

}  // namespace
