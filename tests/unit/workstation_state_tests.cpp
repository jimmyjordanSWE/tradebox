#include "tradebox/workstation/chart_documents.h"
#include "tradebox/workstation/instrument_links.h"
#include "tradebox/workstation/trade_hotkey.h"
#include "tradebox/workstation/profile_codec.h"
#include "tradebox/workstation/profile_store.h"
#include "tradebox/workstation/positions_orders_windows.h"
#include "tradebox/workstation/time_sales_documents.h"
#include "tradebox/workstation/validation.h"
#include "tradebox/workstation/watch_list_documents.h"
#include "tradebox/workstation/watch_list_columns.h"
#include "tradebox/workstation/asset_preferences.h"

#include "time_sales_window.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>

namespace tradebox::workstation {
namespace {

ChartIndicatorState Sma(std::string id, std::uint32_t period) {
    return {
        .definition = {
            .id = std::move(id),
            .calculation =
                core::SimpleMovingAverageCalculation{.period = period},
        },
        .label = "SMA " + std::to_string(period),
    };
}

class TemporaryProfileDirectory {
public:
    TemporaryProfileDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                "tradebox-workstation-state-tests" /
                std::to_string(::GetCurrentProcessId());
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryProfileDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] std::filesystem::path Profile(std::string_view name) const {
        return path_ / (std::string(name) + ".tbw");
    }

private:
    std::filesystem::path path_;
};

TEST(WorkstationState, DefaultsAreValidAndStable) {
    WorkstationState state = WorkstationState::Defaults();
    std::string error;
    EXPECT_TRUE(ValidateAndNormalize(state, error)) << error;
    EXPECT_EQ(state.profile.schema_version, kCurrentSchemaVersion);
    EXPECT_FALSE(state.profile.id.empty());
    EXPECT_TRUE(state.workspace.windows.empty());
    EXPECT_TRUE(state.workspace.charts.empty());
    EXPECT_TRUE(state.workspace.indicator_suites.empty());
    EXPECT_TRUE(state.workspace.chart_drawings.empty());
    EXPECT_EQ(state.workspace.instrument_link_groups.size(), 32U);
    EXPECT_EQ(state.workspace.instrument_link_groups.front().id,
              "instrument-link.red");
    EXPECT_EQ(state.workspace.instrument_link_groups.back().id,
              "instrument-link.white");
}

TEST(PositionsAndOrdersWindows, CreateSeparatePersistentWindows) {
    WorkstationState state = WorkstationState::Defaults();
    ASSERT_TRUE(CreatePositionsWindow(state.workspace));
    ASSERT_TRUE(CreateOrdersWindow(state.workspace));

    const auto positions = state.workspace.windows.find(
        std::string(kPositionsWindowId));
    ASSERT_NE(positions, state.workspace.windows.end());
    EXPECT_EQ(positions->second.kind, "positions");
    EXPECT_EQ(positions->second.title, "Positions");
    ASSERT_TRUE(positions->second.tables.contains(
        std::string(kPositionsTableId)));
    EXPECT_EQ(positions->second.tables.at(std::string(kPositionsTableId))
                  .columns.size(), 7U);

    const auto orders = state.workspace.windows.find(
        std::string(kOrdersWindowId));
    ASSERT_NE(orders, state.workspace.windows.end());
    EXPECT_EQ(orders->second.kind, "orders");
    EXPECT_EQ(orders->second.title, "Orders");
    ASSERT_TRUE(orders->second.tables.contains(std::string(kOrdersTableId)));
    EXPECT_EQ(orders->second.tables.at(std::string(kOrdersTableId))
                  .columns.size(), 11U);

    ASSERT_TRUE(AddPositionColumn(state.workspace, "day_pnl"));
    EXPECT_TRUE(RemovePositionColumn(state.workspace, "day_pnl"));
    EXPECT_FALSE(RemovePositionColumn(state.workspace, "symbol"));
    ASSERT_TRUE(AddOrderColumn(state.workspace, "client_order_id"));
    EXPECT_TRUE(RemoveOrderColumn(state.workspace, "client_order_id"));
    EXPECT_FALSE(RemoveOrderColumn(state.workspace, "symbol"));
    EXPECT_NE(FindPositionColumn("valuation_received_ms"), nullptr);
    EXPECT_NE(FindOrderColumn("filled_at"), nullptr);
    EXPECT_NE(FindOrderColumn("canceled_at"), nullptr);
    EXPECT_NE(FindOrderColumn("failed_at"), nullptr);

    const auto decoded = DecodeProfile(EncodeProfile(state));
    ASSERT_TRUE(decoded) << decoded.error();
    EXPECT_TRUE(decoded->workspace.windows.contains(
        std::string(kPositionsWindowId)));
    EXPECT_TRUE(decoded->workspace.windows.contains(
        std::string(kOrdersWindowId)));
}

TEST(TimeSalesWindow, QueuesOneInitialRecentTradeRequestPerInstrument) {
    WorkstationState state = WorkstationState::Defaults();
    ASSERT_TRUE(CreateTimeSalesDocument(state.workspace));
    TimeSalesDocumentState& document = state.workspace.time_sales.front();
    ASSERT_TRUE(AssignTimeSalesInstrument(
        state.workspace, document.id, "asset:pltr", "PLTR"));

    gui::TimeSalesWindowRenderer renderer;
    application::UiSnapshotQuery query{.as_of_ns = 1'000'000'000'000LL};
    renderer.AppendSnapshotQuery(state.workspace, query);

    ASSERT_EQ(query.market_symbols, std::vector<std::string>({"PLTR"}));
    const auto requests = renderer.ConsumeHistoryRequests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front().document_id, document.id);
    EXPECT_EQ(requests.front().query.instrument_id, "asset:pltr");
    EXPECT_EQ(requests.front().query.symbol, "PLTR");
    EXPECT_EQ(requests.front().query.start_ns, 100'000'000'000LL);
    EXPECT_EQ(requests.front().query.end_ns, 1'000'000'000'000LL);
    EXPECT_TRUE(requests.front().query.include_trades);
    EXPECT_FALSE(requests.front().query.include_quotes);

    renderer.AppendSnapshotQuery(state.workspace, query);
    EXPECT_TRUE(renderer.ConsumeHistoryRequests().empty());
}

TEST(PositionsAndOrdersWindows, ColumnRegistriesContainOnlyUsableDefinitions) {
    const auto assert_usable = [](const auto definitions) {
        ASSERT_FALSE(definitions.empty());
        for (const TableColumnDefinition& definition : definitions) {
            EXPECT_FALSE(definition.id.empty());
            EXPECT_FALSE(definition.label.empty());
            EXPECT_GT(definition.default_width, 0.0f);
        }
    };

    assert_usable(PositionColumnDefinitions());
    assert_usable(OrderColumnDefinitions());
}

TEST(TradeHotkeyWindow, OpensThroughThePersistentWindowSystem) {
    WorkstationState state = WorkstationState::Defaults();
    ASSERT_TRUE(OpenTradeHotkeyWindow(state.workspace));
    const auto window = state.workspace.windows.find(
        std::string(kTradeHotkeyWindowId));
    ASSERT_NE(window, state.workspace.windows.end());
    EXPECT_EQ(window->second.kind, "trade-hotkey");
    EXPECT_TRUE(window->second.open);
}

TEST(WorkstationState, ProfileRoundTripPreservesSemanticState) {
    WorkstationState source = WorkstationState::Defaults();
    source.profile.name = "Research";
    source.application.account_risk_per_trade_percent = 1.25f;
    source.application.max_long_buying_power_percent = 95.0f;
    source.application.max_short_buying_power_percent = 75.0f;
    source.application.watch_list_strong_green_rgba = 0x12345678U;
    source.application.watch_list_light_green_rgba = 0x23456789U;
    source.application.watch_list_light_red_rgba = 0x3456789aU;
    source.application.watch_list_strong_red_rgba = 0x456789abU;
    source.workspace.selected_symbol = "NVDA";
    source.workspace.bracket_drafts.emplace(
        "NVDA", BracketDraftState{.target_percent = 0.75f,
                                   .stop_percent = 0.50f});
    RecordAssetSelection(source.workspace, "alpaca:1");
    RecordAssetSelection(source.workspace, "alpaca:2");
    RecordAssetSelection(source.workspace, "alpaca:1");
    source.workspace.windows.emplace(
        "tool.activity",
        WindowInstanceState{.id = "tool.activity",
                            .kind = "tool",
                            .title = "ACTIVITY",
                            .open = true,
                            .bounds = {40.0f, 40.0f, 900.0f, 600.0f},
                            .selected_tab = "Positions"});
    source.workspace.windows.at("tool.activity").selected_tab = "Orders";
    ASSERT_TRUE(ApplyInstrumentLinkCommand(
        source.workspace,
        RenameInstrumentLinkGroup{
            .group_id = "instrument-link.green",
            .name = "Bullish watch list"}));
    ASSERT_TRUE(ApplyInstrumentLinkCommand(
        source.workspace,
        SelectInstrumentLinkGroupInstrument{
            .group_id = "instrument-link.green",
            .instrument = {.instrument_id = "alpaca:1",
                           .symbol = "NVDA"}}));
    ASSERT_TRUE(ApplyInstrumentLinkCommand(
        source.workspace,
        BindWindowInstrumentLink{
            .window_id = "tool.activity",
            .group_id = "instrument-link.green"}));
    source.workspace.windows.at("tool.activity").tables["orders"].columns = {
        {.id = "symbol", .order = 2, .width = 180.0f, .visible = true,
         .sort_direction = "ascending"},
    };
    source.workspace.chart_defaults.indicators = {Sma("default.sma.20", 20)};
    const auto chart_id = CreateChartDocument(
        source.workspace,
        {.instrument_id = "alpaca:1", .symbol = "NVDA"});
    ASSERT_TRUE(chart_id) << chart_id.error().message;
    ChartDocumentState* chart =
        FindChartDocument(source.workspace, *chart_id);
    ASSERT_NE(chart, nullptr);
    chart->timeframe = "5Min";
    chart->visible_bars = 240;
    chart->indicators.push_back(Sma("chart.sma.50", 50));
    ChartIndicatorState smoothed = Sma("chart.sma.50.twice", 2);
    std::get<core::SimpleMovingAverageCalculation>(
        smoothed.definition.calculation).input =
            core::IndicatorOutputInput{"chart.sma.50", "value"};
    chart->indicators.push_back(std::move(smoothed));
    const auto suite_id = SaveIndicatorSuiteFromChart(
        source.workspace, *chart_id, "Daily moving averages");
    ASSERT_TRUE(suite_id) << suite_id.error().message;
    const auto drawing_price = core::Decimal::Parse("123.45");
    ASSERT_TRUE(drawing_price);
    ASSERT_TRUE(UpsertChartDrawing(
        source.workspace,
        {.id = "drawing.1",
         .instrument_id = "alpaca:1",
         .kind = ChartDrawingKind::HorizontalLine,
         .first = {.time_ns = 100, .price = *drawing_price},
         .label = "Breakout"}));
    const auto watch_list_id = CreateWatchListDocument(source.workspace);
    ASSERT_TRUE(watch_list_id) << watch_list_id.error().message;
    WatchListDocumentState* watch_list =
        FindWatchListDocument(source.workspace, *watch_list_id);
    ASSERT_NE(watch_list, nullptr);
    watch_list->name = "Bullish symbols";
    ASSERT_FALSE(watch_list->rows.empty());
    ASSERT_TRUE(AssignWatchListRowAsset(
        source.workspace, *watch_list_id, watch_list->rows.front().id,
        "alpaca:2", "AMD"));
    const std::string encoded = EncodeProfile(source);
    const auto decoded = DecodeProfile(encoded);
    ASSERT_TRUE(decoded) << decoded.error();
    EXPECT_EQ(decoded->profile.name, "Research");
    EXPECT_FLOAT_EQ(decoded->application.account_risk_per_trade_percent,
                    1.25f);
    EXPECT_FLOAT_EQ(decoded->application.max_long_buying_power_percent,
                    95.0f);
    EXPECT_FLOAT_EQ(decoded->application.max_short_buying_power_percent,
                    75.0f);
    EXPECT_EQ(decoded->application.watch_list_strong_green_rgba,
              0x12345678U);
    EXPECT_EQ(decoded->application.watch_list_light_green_rgba,
              0x23456789U);
    EXPECT_EQ(decoded->application.watch_list_light_red_rgba, 0x3456789aU);
    EXPECT_EQ(decoded->application.watch_list_strong_red_rgba, 0x456789abU);
    EXPECT_EQ(decoded->workspace.selected_symbol, "NVDA");
    ASSERT_TRUE(decoded->workspace.bracket_drafts.contains("NVDA"));
    ASSERT_EQ(decoded->workspace.asset_selection_history.size(), 2U);
    EXPECT_EQ(decoded->workspace.asset_selection_history[0], "alpaca:1");
    EXPECT_EQ(decoded->workspace.asset_selection_history[1], "alpaca:2");
    EXPECT_EQ(decoded->workspace.windows.at("tool.activity").selected_tab,
              "Orders");
    EXPECT_EQ(decoded->workspace.windows.at("tool.activity")
                  .instrument_link_group_id,
              "instrument-link.green");
    ASSERT_EQ(decoded->workspace.watch_lists.size(), 1U);
    EXPECT_EQ(decoded->workspace.active_watch_list_id, *watch_list_id);
    EXPECT_EQ(decoded->workspace.watch_lists.front().name,
              "Bullish symbols");
    ASSERT_EQ(decoded->workspace.watch_lists.front().rows.size(), 1U);
    EXPECT_EQ(decoded->workspace.watch_lists.front().rows.front().symbol,
              "AMD");
    const auto* linked = LinkedInstrumentForWindow(
        decoded->workspace, "tool.activity");
    ASSERT_NE(linked, nullptr);
    EXPECT_EQ(linked->instrument_id, "alpaca:1");
    EXPECT_EQ(FindInstrumentLinkGroup(
                  decoded->workspace, "instrument-link.green")->name,
              "Bullish watch list");
    ASSERT_EQ(decoded->workspace.charts.size(), 1U);
    EXPECT_EQ(decoded->workspace.charts.front().timeframe, "5Min");
    ASSERT_EQ(decoded->workspace.charts.front().indicators.size(), 3U);
    EXPECT_EQ(
        std::get<core::IndicatorOutputInput>(
            std::get<core::SimpleMovingAverageCalculation>(
                decoded->workspace.charts.front().indicators.back()
                    .definition.calculation).input).indicator_id,
        "chart.sma.50");
    EXPECT_TRUE(decoded->workspace.windows.contains(*chart_id));
    ASSERT_EQ(decoded->workspace.indicator_suites.size(), 1U);
    EXPECT_EQ(decoded->workspace.indicator_suites.front().name,
              "Daily moving averages");
    ASSERT_EQ(decoded->workspace.chart_drawings.size(), 1U);
    EXPECT_EQ(decoded->workspace.chart_drawings.front().first.price,
              *drawing_price);
    EXPECT_EQ(EncodeProfile(*decoded), encoded);
}

TEST(WatchListDocuments, ClearsAssignedAssetThroughWorkstationOwner) {
    WorkstationState state = WorkstationState::Defaults();
    const auto document_id = CreateWatchListDocument(state.workspace);
    ASSERT_TRUE(document_id);
    auto* document = FindWatchListDocument(state.workspace, *document_id);
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(AssignWatchListRowAsset(
        state.workspace, *document_id, document->rows.front().id,
        "asset-aapl", "AAPL"));

    ASSERT_TRUE(ClearWatchListRowAsset(
        state.workspace, *document_id, document->rows.front().id));
    EXPECT_TRUE(document->rows.front().instrument_id.empty());
    EXPECT_TRUE(document->rows.front().symbol.empty());
    EXPECT_TRUE(document->rows.front().ticker_input.empty());
}

TEST(WatchListDocuments, MovesRowsUsingStableRowIdentity) {
    WorkstationState state = WorkstationState::Defaults();
    const auto document_id = CreateWatchListDocument(state.workspace);
    ASSERT_TRUE(document_id);
    ASSERT_TRUE(AddWatchListRow(state.workspace, *document_id));
    ASSERT_TRUE(AddWatchListRow(state.workspace, *document_id));
    auto* document = FindWatchListDocument(state.workspace, *document_id);
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->rows.size(), 3U);
    const std::string first_id = document->rows[0].id;
    const std::string second_id = document->rows[1].id;
    const std::string third_id = document->rows[2].id;

    const auto moved = MoveWatchListRow(
        state.workspace, *document_id, first_id, 3U);
    ASSERT_TRUE(moved);
    EXPECT_TRUE(*moved);
    EXPECT_EQ(document->rows[0].id, second_id);
    EXPECT_EQ(document->rows[1].id, third_id);
    EXPECT_EQ(document->rows[2].id, first_id);

    const auto no_op = MoveWatchListRow(
        state.workspace, *document_id, first_id, 3U);
    ASSERT_TRUE(no_op);
    EXPECT_FALSE(*no_op);

    const auto decoded = DecodeProfile(EncodeProfile(state));
    ASSERT_TRUE(decoded);
    const auto* decoded_document = FindWatchListDocument(
        decoded->workspace, *document_id);
    ASSERT_NE(decoded_document, nullptr);
    ASSERT_EQ(decoded_document->rows.size(), 3U);
    EXPECT_EQ(decoded_document->rows[0].id, second_id);
    EXPECT_EQ(decoded_document->rows[1].id, third_id);
    EXPECT_EQ(decoded_document->rows[2].id, first_id);
}

TEST(WatchListDocuments, DeletesRowsThroughWorkstationOwner) {
    WorkstationState state = WorkstationState::Defaults();
    const auto document_id = CreateWatchListDocument(state.workspace);
    ASSERT_TRUE(document_id);
    const auto second = AddWatchListRow(state.workspace, *document_id);
    ASSERT_TRUE(second);
    auto* document = FindWatchListDocument(state.workspace, *document_id);
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->rows.size(), 2U);

    const std::string first_id = document->rows.front().id;
    ASSERT_TRUE(DeleteWatchListRow(
        state.workspace, *document_id, first_id));
    ASSERT_EQ(document->rows.size(), 1U);
    EXPECT_EQ(document->rows.front().id, *second);
    EXPECT_FALSE(DeleteWatchListRow(
        state.workspace, *document_id, first_id));
}

TEST(WatchListDocuments, KeepsOneEmptyDraftRowAtTheEnd) {
    WatchListDocumentState document{
        .id = "watch-list.test",
        .rows = {
            {.id = "empty.first"},
            {.id = "populated", .instrument_id = "asset-aapl", .symbol = "AAPL",
             .ticker_input = "AAPL"},
            {.id = "empty.second"},
        },
    };

    EXPECT_TRUE(EnsureWatchListTrailingEmptyRow(document));
    ASSERT_EQ(document.rows.size(), 2U);
    EXPECT_EQ(document.rows.front().id, "populated");
    EXPECT_EQ(document.rows.back().id, "empty.first");
    EXPECT_FALSE(EnsureWatchListTrailingEmptyRow(document));
}

TEST(WatchListDocuments, RemovesBlankRowsForExplicitAddRowUi) {
    WorkstationState state = WorkstationState::Defaults();
    state.workspace.watchlist.clear();
    ASSERT_TRUE(EnsureDefaultWatchList(state.workspace));
    const auto document = FindWatchListDocument(
        state.workspace, kWatchListDefaultId);
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(AddWatchListRow(state.workspace, kWatchListDefaultId));
    ASSERT_TRUE(AddWatchListRow(state.workspace, kWatchListDefaultId));
    document->rows.front().symbol = "AAPL";
    document->rows.front().instrument_id = "asset-aapl";

    const auto removed = RemoveEmptyWatchListRows(
        state.workspace, kWatchListDefaultId);
    ASSERT_TRUE(removed);
    EXPECT_TRUE(*removed);
    ASSERT_EQ(document->rows.size(), 1U);
    EXPECT_EQ(document->rows.front().symbol, "AAPL");
    EXPECT_FALSE(*RemoveEmptyWatchListRows(
        state.workspace, kWatchListDefaultId));
}

TEST(WatchListDocuments, InitializesAndAddsTableColumns) {
    WorkstationState state = WorkstationState::Defaults();
    const auto created = CreateWatchListDocument(state.workspace);
    ASSERT_TRUE(created) << created.error().message;
    const auto window = state.workspace.windows.find(
        std::string(kWatchListWindowId));
    ASSERT_NE(window, state.workspace.windows.end());
    const auto table = window->second.tables.find(
        std::string(kWatchListTableId));
    ASSERT_NE(table, window->second.tables.end());
    ASSERT_EQ(table->second.columns.size(), 7U);
    EXPECT_EQ(table->second.columns.front().id, "symbol");

    EXPECT_FALSE(AddWatchListColumn(
        state.workspace, *created, WatchListColumnKind::CurrentPrice));
    EXPECT_FALSE(AddWatchListColumn(
        state.workspace, *created, WatchListColumnKind::ChangeFromOpen));
    EXPECT_FALSE(AddWatchListColumn(
        state.workspace, *created, WatchListColumnKind::CurrentPrice));
    ASSERT_EQ(table->second.columns.size(), 7U);
    EXPECT_EQ(table->second.columns[1].id, "current_price");
    EXPECT_EQ(table->second.columns[5].id, "change_from_open");

    ASSERT_TRUE(RemoveWatchListColumn(
        state.workspace, *created, WatchListColumnKind::CurrentPrice));
    table->second.columns.front().order = 6;
    table->second.columns.back().order = 0;
    ASSERT_TRUE(AddWatchListColumn(
        state.workspace, *created, WatchListColumnKind::CurrentPrice));
    const auto newest = std::ranges::find(
        table->second.columns, "current_price",
        &ColumnState::id);
    ASSERT_NE(newest, table->second.columns.end());
    EXPECT_EQ(newest->order, 7);

    EXPECT_FALSE(RemoveWatchListColumn(
        state.workspace, *created, WatchListColumnKind::Symbol));
    ASSERT_TRUE(RemoveWatchListColumn(
        state.workspace, *created, WatchListColumnKind::CurrentPrice));
    ASSERT_EQ(table->second.columns.size(), 6U);
    EXPECT_EQ(table->second.columns[0].id, "symbol");
    EXPECT_EQ(table->second.columns[1].id, "trade_time");

    std::string error;
    EXPECT_TRUE(ValidateAndNormalize(state, error)) << error;
    const auto decoded = DecodeProfile(EncodeProfile(state));
    ASSERT_TRUE(decoded) << decoded.error();
    const auto decoded_window = decoded->workspace.windows.find(
        std::string(kWatchListWindowId));
    ASSERT_NE(decoded_window, decoded->workspace.windows.end());
    EXPECT_EQ(decoded_window->second.tables.at(std::string(kWatchListTableId))
                  .columns.size(),
              6U);
}

TEST(WatchListDocuments, MigratesCloseColumnsToOpenColumns) {
    WorkstationState state = WorkstationState::Defaults();
    ASSERT_TRUE(EnsureWatchListWindow(state.workspace));
    auto& table = state.workspace.windows.at(std::string(kWatchListWindowId))
                      .tables[std::string(kWatchListTableId)];
    table.columns = {
        {.id = "symbol", .order = 0, .width = 100.0f, .visible = true},
        {.id = "change_from_close", .order = 1, .width = 120.0f,
         .visible = true},
    };

    ASSERT_TRUE(EnsureWatchListWindow(state.workspace));
    ASSERT_EQ(table.columns.size(), 2U);
    EXPECT_EQ(table.columns[1].id, "change_from_open");
    EXPECT_TRUE(FindWatchListColumn("change_from_open"));
}

TEST(WatchListDocuments, SavesAndManagesOneNamedDocumentLibrary) {
    WorkstationState state = WorkstationState::Defaults();
    WatchListDocumentState draft{
        .id = std::string(kWatchListDraftId),
        .name = "Momentum",
        .rows = {{.id = "row.momentum"}},
    };

    const auto saved = SaveWatchListDocument(state.workspace, draft);
    ASSERT_TRUE(saved) << saved.error().message;
    EXPECT_NE(*saved, kWatchListDraftId);
    EXPECT_EQ(state.workspace.active_watch_list_id, *saved);
    ASSERT_EQ(state.workspace.watch_lists.size(), 1U);
    EXPECT_EQ(state.workspace.watch_lists.front().name, "Momentum");
    EXPECT_TRUE(state.workspace.windows.contains(std::string(kWatchListWindowId)));
    EXPECT_EQ(state.workspace.windows.size(), 1U);

    const auto duplicate = SaveWatchListDocument(state.workspace, draft);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().message,
              "watch list name is already in use");

    ASSERT_TRUE(RenameWatchListDocument(
        state.workspace, *saved, "Breakouts"));
    EXPECT_EQ(FindWatchListDocument(state.workspace, *saved)->name,
              "Breakouts");
    ASSERT_TRUE(OpenWatchListDocument(state.workspace, *saved));
    EXPECT_EQ(state.workspace.active_watch_list_id, *saved);

    ASSERT_TRUE(DeleteWatchListDocument(state.workspace, *saved));
    EXPECT_TRUE(state.workspace.watch_lists.empty());
    EXPECT_TRUE(state.workspace.active_watch_list_id.empty());
    EXPECT_TRUE(state.workspace.windows.contains(std::string(kWatchListWindowId)));
}

TEST(WatchListDocuments, ReusesOnePersistentDefaultWatchList) {
    WorkstationState state = WorkstationState::Defaults();

    const auto first = EnsureDefaultWatchList(state.workspace);
    ASSERT_TRUE(first) << first.error().message;
    const auto second = EnsureDefaultWatchList(state.workspace);
    ASSERT_TRUE(second) << second.error().message;

    EXPECT_EQ(*first, kWatchListDefaultId);
    EXPECT_EQ(*second, kWatchListDefaultId);
    ASSERT_EQ(state.workspace.watch_lists.size(), 1U);
    EXPECT_EQ(state.workspace.watch_lists.front().name, "Default");
    ASSERT_EQ(state.workspace.watch_lists.front().rows.size(), 4U);
    EXPECT_EQ(state.workspace.watch_lists.front().rows[0].ticker_input, "AMD");
    EXPECT_EQ(state.workspace.watch_lists.front().rows[1].ticker_input, "AAPL");
    EXPECT_EQ(state.workspace.watch_lists.front().rows[2].ticker_input, "NVDA");
    EXPECT_EQ(state.workspace.watch_lists.front().rows[3].ticker_input, "SPY");
    EXPECT_TRUE(state.workspace.watchlist.empty());
    EXPECT_EQ(state.workspace.active_watch_list_id, kWatchListDefaultId);
    EXPECT_TRUE(state.workspace.windows.empty());
    ASSERT_TRUE(OpenWatchListDocument(
        state.workspace, kWatchListDefaultId));
    EXPECT_TRUE(state.workspace.windows.at(std::string(kWatchListWindowId)).open);
    EXPECT_FALSE(DeleteWatchListDocument(
        state.workspace, kWatchListDefaultId));
}

TEST(WatchListDocuments, MigratesLegacySymbolsOnlyOnce) {
    WorkstationState state = WorkstationState::Defaults();
    state.workspace.watchlist = {"AAPL", "MSFT"};
    state.workspace.watch_lists.push_back({
        .id = std::string(kWatchListDefaultId),
        .name = "Default",
    });

    ASSERT_TRUE(EnsureDefaultWatchList(state.workspace));
    const auto first = state.workspace.watch_lists.front().rows;
    ASSERT_TRUE(EnsureDefaultWatchList(state.workspace));

    ASSERT_EQ(state.workspace.watch_lists.size(), 1U);
    EXPECT_EQ(state.workspace.watch_lists.front().rows.size(), first.size());
    EXPECT_TRUE(state.workspace.watchlist.empty());
}

TEST(WatchListDocuments, DraftRowsUseStableIdentityAndCanBeEditedBeforeSaving) {
    WatchListDocumentState draft{
        .id = std::string(kWatchListDraftId),
        .name = "Momentum",
    };
    const auto first = AddWatchListRow(draft);
    const auto second = AddWatchListRow(draft);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_NE(*first, *second);

    ASSERT_TRUE(AssignWatchListRowAsset(
        draft, *first, "asset:tsla", "TSLA"));
    const auto assigned = std::ranges::find(
        draft.rows, *first, &WatchListRowState::id);
    ASSERT_NE(assigned, draft.rows.end());
    EXPECT_EQ(assigned->symbol, "TSLA");
    ASSERT_TRUE(ClearWatchListRowAsset(draft, *first));
    EXPECT_TRUE(draft.rows.front().symbol.empty());
}

TEST(WorkstationState, StoreRoundTripsThroughOneProfileFile) {
    TemporaryProfileDirectory temporary;
    const std::filesystem::path path = temporary.Profile("research");
    ProfileStore writer;
    std::string error;
    ASSERT_TRUE(writer.Open(path, false, error)) << error;
    WorkstationState state = WorkstationState::Defaults();
    state.profile.name = "Research";
    ASSERT_TRUE(writer.SaveNow(state, error)) << error;
    writer.Close();

    ProfileStore reader;
    ASSERT_TRUE(reader.Open(path, true, error)) << error;
    const auto restored = reader.Load(path);
    ASSERT_TRUE(restored) << restored.error();
    EXPECT_EQ(restored->profile.name, "Research");
    reader.Close();
}

TEST(TimeSalesDocuments, CreateAssignAndRoundTripThroughProfile) {
    WorkstationState state = WorkstationState::Defaults();
    const auto created = CreateTimeSalesDocument(state.workspace);
    ASSERT_TRUE(created);
    ASSERT_TRUE(AssignTimeSalesInstrument(
        state.workspace, *created, "asset-pltr", "PLTR"));
    state.workspace.time_sales_table.columns = {
        {.id = "time", .order = 0, .width = 110.0f, .visible = true},
        {.id = "price", .order = 1, .width = 110.0f, .visible = true},
    };

    const auto decoded = DecodeProfile(EncodeProfile(state));
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded->workspace.time_sales.size(), 1U);
    EXPECT_EQ(decoded->workspace.time_sales.front().instrument_id, "asset-pltr");
    EXPECT_EQ(decoded->workspace.time_sales.front().symbol, "PLTR");
    EXPECT_TRUE(decoded->workspace.windows.contains(*created));
    ASSERT_EQ(decoded->workspace.time_sales_table.columns.size(), 2U);
    EXPECT_EQ(decoded->workspace.time_sales_table.columns.front().id, "time");
}

TEST(WorkstationState, RejectsDuplicateDocumentIdentity) {
    WorkstationState state = WorkstationState::Defaults();
    state.workspace.charts = {
        {.id = "chart:1", .instrument_id = "asset:amd", .symbol = "AMD"},
        {.id = "chart:1", .instrument_id = "asset:aapl", .symbol = "AAPL"},
    };
    std::string error;
    EXPECT_FALSE(ValidateAndNormalize(state, error));
    EXPECT_FALSE(error.empty());
}

TEST(ChartDocuments, ClosePreservesDocumentAndDeleteRemovesOnlyWindowState) {
    WorkstationState state = WorkstationState::Defaults();
    const auto created = CreateChartDocument(
        state.workspace,
        {.instrument_id = "asset:amd", .symbol = "AMD"});
    ASSERT_TRUE(created) << created.error().message;
    ASSERT_TRUE(CloseChartDocument(state.workspace, *created));
    EXPECT_NE(FindChartDocument(state.workspace, *created), nullptr);
    EXPECT_FALSE(state.workspace.windows.at(*created).open);
    ASSERT_TRUE(ReopenChartDocument(state.workspace, *created));
    EXPECT_TRUE(state.workspace.windows.at(*created).open);
    ASSERT_TRUE(DeleteChartDocument(state.workspace, *created));
    EXPECT_EQ(FindChartDocument(state.workspace, *created), nullptr);
    EXPECT_FALSE(state.workspace.windows.contains(*created));
}

TEST(ChartDocuments, OpensUnassignedAndAcceptsResolvedTickerLater) {
    WorkstationState state = WorkstationState::Defaults();
    auto created = CreateChartDocument(state.workspace);
    ASSERT_TRUE(created) << created.error().message;
    const ChartDocumentState* chart =
        FindChartDocument(state.workspace, *created);
    ASSERT_NE(chart, nullptr);
    EXPECT_TRUE(chart->instrument_id.empty());
    EXPECT_TRUE(chart->symbol.empty());
    EXPECT_TRUE(chart->ticker_input.empty());

    auto assigned = AssignChartInstrument(
        state.workspace, *created, "asset-msft", "MSFT");
    ASSERT_TRUE(assigned) << assigned.error().message;
    chart = FindChartDocument(state.workspace, *created);
    ASSERT_NE(chart, nullptr);
    EXPECT_EQ(chart->instrument_id, "asset-msft");
    EXPECT_EQ(chart->symbol, "MSFT");
    EXPECT_EQ(chart->ticker_input, "MSFT");
    EXPECT_EQ(chart->range_anchor_ns, 0);
    std::string validation_error;
    EXPECT_TRUE(ValidateAndNormalize(state, validation_error))
        << validation_error;
}

TEST(ChartDocuments, SuitesAndDefaultsAreCopiedNotLinked) {
    WorkstationState state = WorkstationState::Defaults();
    state.workspace.chart_defaults.indicators = {Sma("default.sma.20", 20)};
    const auto first = CreateChartDocument(
        state.workspace,
        {.instrument_id = "asset:amd", .symbol = "AMD"});
    ASSERT_TRUE(first) << first.error().message;
    ChartDocumentState* chart = FindChartDocument(state.workspace, *first);
    ASSERT_NE(chart, nullptr);
    EXPECT_NE(chart->indicators.front().definition.id, "default.sma.20");
    chart->indicators = {Sma("chart.sma.50", 50)};
    ChartIndicatorState smoothed = Sma("chart.sma.50.twice", 2);
    std::get<core::SimpleMovingAverageCalculation>(
        smoothed.definition.calculation).input =
            core::IndicatorOutputInput{"chart.sma.50", "value"};
    chart->indicators.push_back(std::move(smoothed));
    const auto suite = SaveIndicatorSuiteFromChart(
        state.workspace, *first, "Daily moving averages");
    ASSERT_TRUE(suite) << suite.error().message;

    std::get<core::SimpleMovingAverageCalculation>(
        chart->indicators.front().definition.calculation).period = 100;
    EXPECT_EQ(std::get<core::SimpleMovingAverageCalculation>(
                  state.workspace.indicator_suites.front()
                      .indicators.front().definition.calculation).period,
              50U);
    ASSERT_TRUE(ApplyIndicatorSuite(state.workspace, *first, *suite));
    EXPECT_EQ(std::get<core::SimpleMovingAverageCalculation>(
                  chart->indicators.front().definition.calculation).period,
              50U);
    EXPECT_NE(chart->indicators.front().definition.id,
              state.workspace.indicator_suites.front()
                  .indicators.front().definition.id);
    ASSERT_EQ(chart->indicators.size(), 2U);
    EXPECT_EQ(
        std::get<core::IndicatorOutputInput>(
            std::get<core::SimpleMovingAverageCalculation>(
                chart->indicators.back().definition.calculation).input)
            .indicator_id,
        chart->indicators.front().definition.id);
    ASSERT_TRUE(SetChartDefaultsFromDocument(state.workspace, *first));

    const auto second = CreateChartDocument(
        state.workspace,
        {.instrument_id = "asset:nvda", .symbol = "NVDA"});
    ASSERT_TRUE(second) << second.error().message;
    const ChartDocumentState* second_chart =
        FindChartDocument(state.workspace, *second);
    ASSERT_NE(second_chart, nullptr);
    EXPECT_EQ(std::get<core::SimpleMovingAverageCalculation>(
                  second_chart->indicators.front()
                      .definition.calculation).period,
              50U);
    chart = FindChartDocument(state.workspace, *first);
    ASSERT_NE(chart, nullptr);
    EXPECT_NE(second_chart->indicators.front().definition.id,
              chart->indicators.front().definition.id);
    EXPECT_EQ(
        std::get<core::IndicatorOutputInput>(
            std::get<core::SimpleMovingAverageCalculation>(
                second_chart->indicators.back()
                    .definition.calculation).input).indicator_id,
        second_chart->indicators.front().definition.id);
}

TEST(ChartDocuments, OwnsIndicatorInstanceLifecycle) {
    WorkstationState state = WorkstationState::Defaults();
    const auto chart = CreateChartDocument(state.workspace);
    ASSERT_TRUE(chart) << chart.error().message;
    ChartIndicatorState indicator = Sma({}, 20);
    const auto indicator_id = AddChartIndicator(
        state.workspace, *chart, std::move(indicator));
    ASSERT_TRUE(indicator_id) << indicator_id.error().message;
    EXPECT_FALSE(indicator_id->empty());
    ChartIndicatorState dependent = Sma({}, 5);
    std::get<core::SimpleMovingAverageCalculation>(
        dependent.definition.calculation).input =
            core::IndicatorOutputInput{*indicator_id, "value"};
    const auto dependent_id = AddChartIndicator(
        state.workspace, *chart, std::move(dependent));
    ASSERT_TRUE(dependent_id) << dependent_id.error().message;
    EXPECT_FALSE(RemoveChartIndicator(
        state.workspace, *chart, *indicator_id));
    EXPECT_TRUE(RemoveChartIndicator(
        state.workspace, *chart, *dependent_id));
    EXPECT_TRUE(RemoveChartIndicator(
        state.workspace, *chart, *indicator_id));
}

TEST(ChartDocuments, DrawingsAreSharedByStableInstrumentIdentity) {
    WorkstationState state = WorkstationState::Defaults();
    const auto price = core::Decimal::Parse("99.50");
    ASSERT_TRUE(price);
    const auto created = CreateChartDrawing(
        state.workspace,
        {.instrument_id = "asset:amd",
         .kind = ChartDrawingKind::HorizontalLine,
         .first = {.time_ns = 5, .price = *price}});
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_FALSE(created->empty());
    ASSERT_TRUE(UpsertChartDrawing(
        state.workspace,
        {.id = "drawing.amd.1",
         .instrument_id = "asset:amd",
         .kind = ChartDrawingKind::HorizontalLine,
         .first = {.time_ns = 10, .price = *price}}));
    EXPECT_EQ(DrawingsForInstrument(state.workspace, "asset:amd").size(), 2U);
    EXPECT_TRUE(DrawingsForInstrument(state.workspace, "AMD").empty());
    EXPECT_TRUE(DeleteChartDrawing(state.workspace, "drawing.amd.1"));
    EXPECT_TRUE(DeleteChartDrawing(state.workspace, *created));
}

TEST(WorkstationState, RejectsUnsupportedProfileSchema) {
    WorkstationState state = WorkstationState::Defaults();
    std::string encoded = EncodeProfile(state);
    const std::string current =
        "schema_version = " + std::to_string(kCurrentSchemaVersion);
    const std::size_t version = encoded.find(current);
    ASSERT_NE(version, std::string::npos);
    encoded.replace(version, current.size(), "schema_version = 999");
    const auto decoded = DecodeProfile(encoded);
    EXPECT_FALSE(decoded);
}

TEST(WorkstationState, MigratesSchemaOneWithDefaultLinkGroups) {
    const auto decoded = DecodeProfile(
        "[profile]\n"
        "schema_version = 1\n"
        "id = \"legacy-profile\"\n"
        "name = \"Legacy\"\n");
    ASSERT_TRUE(decoded) << decoded.error();
    EXPECT_EQ(decoded->profile.schema_version, kCurrentSchemaVersion);
    EXPECT_EQ(decoded->workspace.instrument_link_groups.size(), 32U);
    EXPECT_EQ(decoded->workspace.instrument_link_groups[11].id,
              "instrument-link.blue");
}

TEST(WorkstationState, MigratesLegacyHotkeyPositionCapAway) {
    const auto decoded = DecodeProfile(
        "[profile]\n"
        "schema_version = 5\n"
        "id = \"legacy-hotkey-profile\"\n"
        "name = \"Legacy\"\n\n"
        "[bracket_drafts.AMD]\n"
        "maximum_position_percent_of_account = 35.000\n");
    ASSERT_TRUE(decoded) << decoded.error();
    ASSERT_TRUE(decoded->workspace.bracket_drafts.contains("AMD"));
    EXPECT_FLOAT_EQ(decoded->workspace.bracket_drafts.at("AMD")
                        .target_percent,
                    1.0f);
}

TEST(WorkstationState, RemovesDormantOrderTicketDataFromLegacyProfiles) {
    const auto decoded = DecodeProfile(
        "[profile]\n"
        "schema_version = 6\n"
        "id = \"legacy-order-ticket-profile\"\n"
        "name = \"Legacy\"\n\n"
        "[windows.\"order-ticket.window\"]\n"
        "id = \"order-ticket.window\"\n"
        "kind = \"order-ticket\"\n"
        "title = \"Order Ticket\"\n"
        "open = true\n\n"
        "[[order_tickets]]\n"
        "id = \"order-ticket:legacy\"\n"
        "symbol = \"PLTR\"\n");
    ASSERT_TRUE(decoded) << decoded.error();
    EXPECT_FALSE(decoded->workspace.windows.contains("order-ticket.window"));
    EXPECT_EQ(EncodeProfile(*decoded).find("order_tickets"), std::string::npos);
}

TEST(InstrumentLinks, CommandsPreserveLocalWindowStateAndPublishOneContext) {
    WorkstationState state = WorkstationState::Defaults();
    state.workspace.windows.emplace(
        "chart:1",
        WindowInstanceState{.id = "chart:1", .kind = "chart"});
    ASSERT_TRUE(ApplyInstrumentLinkCommand(
        state.workspace,
        BindWindowInstrumentLink{.window_id = "chart:1",
                                 .group_id = "instrument-link.blue"}));
    EXPECT_EQ(LinkedInstrumentForWindow(state.workspace, "chart:1"),
              nullptr);
    ASSERT_TRUE(ApplyInstrumentLinkCommand(
        state.workspace,
        SelectInstrumentLinkGroupInstrument{
            .group_id = "instrument-link.blue",
            .instrument = {.instrument_id = "asset:nvda",
                           .symbol = "NVDA"}}));
    const auto* selected = LinkedInstrumentForWindow(
        state.workspace, "chart:1");
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->symbol, "NVDA");
    ASSERT_TRUE(ApplyInstrumentLinkCommand(
        state.workspace,
        ClearInstrumentLinkGroupInstrument{
            .group_id = "instrument-link.blue"}));
    EXPECT_EQ(LinkedInstrumentForWindow(state.workspace, "chart:1"),
              nullptr);
}

}  // namespace
}  // namespace tradebox::workstation
