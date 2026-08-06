#include "tradebox/workstation/chart_documents.h"
#include "tradebox/workstation/instrument_links.h"
#include "tradebox/workstation/profile_codec.h"
#include "tradebox/workstation/profile_store.h"
#include "tradebox/workstation/validation.h"

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
    EXPECT_TRUE(state.workspace.order_tickets.empty());
    EXPECT_EQ(state.workspace.instrument_link_groups.size(), 32U);
    EXPECT_EQ(state.workspace.instrument_link_groups.front().id,
              "instrument-link.red");
    EXPECT_EQ(state.workspace.instrument_link_groups.back().id,
              "instrument-link.white");
}

TEST(WorkstationState, ProfileRoundTripPreservesSemanticState) {
    WorkstationState source = WorkstationState::Defaults();
    source.profile.name = "Research";
    source.workspace.selected_symbol = "NVDA";
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
    const std::string encoded = EncodeProfile(source);
    const auto decoded = DecodeProfile(encoded);
    ASSERT_TRUE(decoded) << decoded.error();
    EXPECT_EQ(decoded->profile.name, "Research");
    EXPECT_EQ(decoded->workspace.selected_symbol, "NVDA");
    EXPECT_EQ(decoded->workspace.windows.at("tool.activity").selected_tab,
              "Orders");
    EXPECT_EQ(decoded->workspace.windows.at("tool.activity")
                  .instrument_link_group_id,
              "instrument-link.green");
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
