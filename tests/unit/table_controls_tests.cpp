#include "gui_controls.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace tradebox::gui {
namespace {

TEST(TableControls, OrdersOnlyVisibleColumnsByPersistentSemanticOrder) {
    workstation::PersistentTableState table{
        .columns = {
            {.id = "last", .order = 2, .visible = true},
            {.id = "hidden", .order = 0, .visible = false},
            {.id = "symbol", .order = 0, .visible = true},
            {.id = "qty", .order = 1, .visible = true},
        }};

    const auto columns = OrderedVisibleTableColumns(table);

    ASSERT_EQ(columns.size(), 3U);
    EXPECT_EQ(columns[0]->id, "symbol");
    EXPECT_EQ(columns[1]->id, "qty");
    EXPECT_EQ(columns[2]->id, "last");
}

TEST(TableControls, PersistsDisplayOrderWithoutLosingHiddenColumns) {
    workstation::PersistentTableState table{
        .columns = {
            {.id = "symbol", .order = 0, .visible = true},
            {.id = "hidden", .order = 1, .visible = false},
            {.id = "qty", .order = 2, .visible = true},
        }};
    const std::vector<std::string> display_ids{"qty", "symbol"};

    EXPECT_TRUE(PersistTableColumnOrder(table, display_ids));
    ASSERT_EQ(table.columns.size(), 3U);
    EXPECT_EQ(table.columns[0].id, "qty");
    EXPECT_EQ(table.columns[1].id, "symbol");
    EXPECT_EQ(table.columns[2].id, "hidden");
    EXPECT_EQ(table.columns[0].order, 0);
    EXPECT_EQ(table.columns[1].order, 1);
    EXPECT_EQ(table.columns[2].order, 2);
}

TEST(TableControls, RejectsIncompleteOrUnknownDisplayOrder) {
    workstation::PersistentTableState table{
        .columns = {
            {.id = "symbol", .order = 0, .visible = true},
            {.id = "qty", .order = 1, .visible = true},
        }};
    const workstation::PersistentTableState original = table;
    const std::vector<std::string> incomplete{"symbol"};
    const std::vector<std::string> unknown{"symbol", "missing"};

    EXPECT_FALSE(PersistTableColumnOrder(table, incomplete));
    EXPECT_FALSE(PersistTableColumnOrder(table, unknown));
    EXPECT_EQ(table.columns[0].id, original.columns[0].id);
    EXPECT_EQ(table.columns[1].id, original.columns[1].id);
}

TEST(TableControls, ExtractsSemanticIdFromImGuiColumnLabel) {
    EXPECT_EQ(TableColumnIdFromLabel("Shares###qty"), "qty");
    EXPECT_EQ(TableColumnIdFromLabel("symbol"), "symbol");
    EXPECT_TRUE(TableColumnIdFromLabel(nullptr).empty());
}

}  // namespace
}  // namespace tradebox::gui
