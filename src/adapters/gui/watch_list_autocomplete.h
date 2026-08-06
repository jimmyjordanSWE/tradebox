#pragma once

#include <algorithm>
#include <cstddef>

namespace tradebox::gui {

inline int SelectedWatchListAutocompleteMatch(
    int highlighted_match, std::size_t match_count) noexcept {
    if (match_count == 0U) return -1;
    const int last_match = static_cast<int>(match_count - 1U);
    return std::clamp(highlighted_match, 0, last_match);
}

inline int MoveWatchListAutocompleteMatch(
    int highlighted_match, std::size_t match_count, int direction) noexcept {
    const int selected_match =
        SelectedWatchListAutocompleteMatch(highlighted_match, match_count);
    if (selected_match < 0 || direction == 0) return selected_match;
    const int next_match = selected_match + (direction > 0 ? 1 : -1);
    return std::clamp(next_match, 0,
                      static_cast<int>(match_count - 1U));
}

}  // namespace tradebox::gui
