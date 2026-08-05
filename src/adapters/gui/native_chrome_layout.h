#pragma once

#include <array>

namespace tradebox::ui::win32 {

[[nodiscard]] constexpr bool IsTitleBarDragPoint(
    int x, int y, int title_bar_height, int interactive_left_width,
    int window_width, int controls_width) noexcept {
    return y >= 0 && y < title_bar_height &&
           x >= interactive_left_width &&
           x < window_width - controls_width;
}

[[nodiscard]] constexpr std::array<char, 6> FormatHourMinute(
    unsigned int hour, unsigned int minute) noexcept {
    return {
        static_cast<char>('0' + (hour / 10U) % 10U),
        static_cast<char>('0' + hour % 10U),
        ':',
        static_cast<char>('0' + (minute / 10U) % 10U),
        static_cast<char>('0' + minute % 10U),
        '\0'};
}

}  // namespace tradebox::ui::win32
