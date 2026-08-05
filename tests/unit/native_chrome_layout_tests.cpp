#include "native_chrome_layout.h"

#include <gtest/gtest.h>

namespace {

using tradebox::ui::win32::IsTitleBarDragPoint;
using tradebox::ui::win32::FormatHourMinute;

TEST(NativeChromeLayoutTest, UsesEmptyLeftEdgeAsDragRegion) {
    EXPECT_TRUE(IsTitleBarDragPoint(0, 0, 35, 0, 1000, 218));
    EXPECT_TRUE(IsTitleBarDragPoint(39, 34, 35, 0, 1000, 218));
}

TEST(NativeChromeLayoutTest, MakesUnusedTitleBarAreaDraggable) {
    EXPECT_TRUE(IsTitleBarDragPoint(40, 0, 35, 0, 1000, 218));
    EXPECT_TRUE(IsTitleBarDragPoint(781, 34, 35, 0, 1000, 218));
}

TEST(NativeChromeLayoutTest, LeavesToolAndCaptionControlsInteractive) {
    EXPECT_FALSE(IsTitleBarDragPoint(782, 0, 35, 0, 1000, 218));
    EXPECT_FALSE(IsTitleBarDragPoint(999, 34, 35, 0, 1000, 218));
}

TEST(NativeChromeLayoutTest, RejectsPointsOutsideTitleBar) {
    EXPECT_FALSE(IsTitleBarDragPoint(40, -1, 35, 0, 1000, 218));
    EXPECT_FALSE(IsTitleBarDragPoint(40, 35, 35, 0, 1000, 218));
}

TEST(NativeChromeLayoutTest, FormatsMarketClockWithLeadingZeroes) {
    EXPECT_STREQ(FormatHourMinute(9, 5).data(), "09:05");
    EXPECT_STREQ(FormatHourMinute(16, 0).data(), "16:00");
}

}  // namespace
