#include "tradebox/workstation/profile_codec.h"

#include "tradebox/workstation/instrument_links.h"
#include "tradebox/workstation/validation.h"
#include "tradebox/workstation/watch_list_documents.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iomanip>
#include <stdexcept>
#include <sstream>

namespace tradebox::workstation {
namespace {

constexpr std::array<std::string_view, 4> kSchemaSevenSeededWatchList{
    "AMD", "AAPL", "NVDA", "SPY"};

bool MatchesSchemaSevenSeed(
    const std::vector<WatchListRowState>& rows) {
    if (rows.size() < kSchemaSevenSeededWatchList.size()) return false;
    for (std::size_t index = 0;
         index < kSchemaSevenSeededWatchList.size(); ++index) {
        const WatchListRowState& row = rows[index];
        if (!row.instrument_id.empty() || !row.symbol.empty() ||
            row.ticker_input != kSchemaSevenSeededWatchList[index])
            return false;
    }
    return true;
}

void RemoveSchemaSevenWatchListSeed(WorkspaceState& workspace) {
    if (workspace.watchlist.size() == kSchemaSevenSeededWatchList.size() &&
        std::equal(workspace.watchlist.begin(), workspace.watchlist.end(),
                   kSchemaSevenSeededWatchList.begin()))
        workspace.watchlist.clear();

    const auto document = std::ranges::find(
        workspace.watch_lists, kWatchListDefaultId,
        &WatchListDocumentState::id);
    if (document == workspace.watch_lists.end() ||
        !MatchesSchemaSevenSeed(document->rows))
        return;
    document->rows.erase(
        document->rows.begin(),
        document->rows.begin() +
            static_cast<std::ptrdiff_t>(kSchemaSevenSeededWatchList.size()));
}

std::string Quote(std::string_view value) {
    std::string output{"\""};
    for (const char character : value) {
        switch (character) {
            case '\\': output += "\\\\"; break;
            case '\"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output += character; break;
        }
    }
    output += '"';
    return output;
}

std::string Key(std::string_view value) { return Quote(value); }

template <typename T>
T ValueOr(const toml::table& table, std::string_view key, T fallback) {
    return table[key].value_or(std::move(fallback));
}

LogicalRect ReadRect(const toml::table& table, LogicalRect fallback) {
    fallback.x = ValueOr(table, "x", fallback.x);
    fallback.y = ValueOr(table, "y", fallback.y);
    fallback.width = ValueOr(table, "width", fallback.width);
    fallback.height = ValueOr(table, "height", fallback.height);
    return fallback;
}

void WriteRect(std::ostringstream& output, const LogicalRect& bounds) {
    output << "x = " << bounds.x << '\n';
    output << "y = " << bounds.y << '\n';
    output << "width = " << bounds.width << '\n';
    output << "height = " << bounds.height << '\n';
}

std::vector<std::string> ReadStringArray(const toml::table& table,
                                         std::string_view key) {
    std::vector<std::string> values;
    const toml::array* array = table[key].as_array();
    if (array == nullptr) return values;
    for (const toml::node& value : *array) {
        if (const auto text = value.value<std::string>()) values.push_back(*text);
    }
    return values;
}

void WriteStringArray(std::ostringstream& output,
                      const std::vector<std::string>& values) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output << ", ";
        output << Quote(values[index]);
    }
    output << ']';
}

const char* FeedName(core::MarketDataFeed feed) {
    switch (feed) {
        case core::MarketDataFeed::Unknown: return "unknown";
        case core::MarketDataFeed::Iex: return "iex";
        case core::MarketDataFeed::Sip: return "sip";
    }
    return "unknown";
}

core::MarketDataFeed ReadFeed(const toml::table& table,
                              core::MarketDataFeed fallback) {
    const std::string value = ValueOr(
        table, "feed", std::string(FeedName(fallback)));
    if (value == "unknown") return core::MarketDataFeed::Unknown;
    if (value == "iex") return core::MarketDataFeed::Iex;
    if (value == "sip") return core::MarketDataFeed::Sip;
    throw std::runtime_error("Unsupported chart market-data feed: " + value);
}

InstrumentLinkColor ReadInstrumentLinkColor(const toml::table& table) {
    const std::string value = ValueOr(table, "color", std::string{});
    for (std::size_t index = 0; index < kInstrumentLinkGroupCount; ++index) {
        const auto color = static_cast<InstrumentLinkColor>(index);
        if (value == InstrumentLinkColorName(color)) return color;
    }
    throw std::runtime_error("Unsupported instrument link color: " + value);
}

const char* AdjustmentName(core::BarAdjustment adjustment) {
    switch (adjustment) {
        case core::BarAdjustment::Raw: return "raw";
        case core::BarAdjustment::Split: return "split";
        case core::BarAdjustment::Dividend: return "dividend";
        case core::BarAdjustment::All: return "all";
    }
    return "raw";
}

core::BarAdjustment ReadAdjustment(
    const toml::table& table, core::BarAdjustment fallback) {
    const std::string value = ValueOr(
        table, "adjustment", std::string(AdjustmentName(fallback)));
    if (value == "raw") return core::BarAdjustment::Raw;
    if (value == "split") return core::BarAdjustment::Split;
    if (value == "dividend") return core::BarAdjustment::Dividend;
    if (value == "all") return core::BarAdjustment::All;
    throw std::runtime_error("Unsupported chart adjustment: " + value);
}

const char* CalculationName(const core::IndicatorCalculation& calculation) {
    return std::holds_alternative<
               core::SimpleMovingAverageCalculation>(calculation)
               ? "sma"
               : "ema";
}

core::IndicatorCalculation ReadCalculation(const toml::table& table) {
    const std::string value = ValueOr(
        table, "kind", std::string{"sma"});
    const auto period = static_cast<std::uint32_t>(
        std::max<std::int64_t>(
            0, ValueOr(table, "period", std::int64_t{20})));
    if (value == "sma")
        return core::SimpleMovingAverageCalculation{.period = period};
    if (value == "ema")
        return core::ExponentialMovingAverageCalculation{.period = period};
    throw std::runtime_error("Unsupported indicator kind: " + value);
}

const char* BarFieldName(core::BarSeriesField field) {
    switch (field) {
        case core::BarSeriesField::Open: return "open";
        case core::BarSeriesField::High: return "high";
        case core::BarSeriesField::Low: return "low";
        case core::BarSeriesField::Close: return "close";
        case core::BarSeriesField::Volume: return "volume";
    }
    return "close";
}

core::BarSeriesField ReadBarField(const toml::table& table) {
    const std::string value = ValueOr(
        table, "input_bar_field",
        ValueOr(table, "source", std::string{"close"}));
    if (value == "open") return core::BarSeriesField::Open;
    if (value == "high") return core::BarSeriesField::High;
    if (value == "low") return core::BarSeriesField::Low;
    if (value == "close") return core::BarSeriesField::Close;
    if (value == "volume") return core::BarSeriesField::Volume;
    throw std::runtime_error("Unsupported indicator bar input: " + value);
}

void WriteIndicators(std::ostringstream& output, std::string_view path,
                     const std::vector<ChartIndicatorState>& indicators) {
    for (const ChartIndicatorState& indicator : indicators) {
        output << "[[" << path << "]]\n";
        output << "id = " << Quote(indicator.definition.id) << '\n';
        output << "kind = "
               << Quote(CalculationName(indicator.definition.calculation))
               << '\n';
        std::visit(
            [&](const auto& calculation) {
                output << "period = " << calculation.period << '\n';
            },
            indicator.definition.calculation);
        const core::IndicatorInput& input = std::visit(
            [](const auto& calculation) -> const core::IndicatorInput& {
                return calculation.input;
            },
            indicator.definition.calculation);
        if (const auto* bar =
                std::get_if<core::BarSeriesInput>(&input)) {
            output << "input_type = \"bar\"\n";
            output << "input_bar_field = "
                   << Quote(BarFieldName(bar->field)) << '\n';
        } else {
            const auto& reference = std::get<core::IndicatorOutputInput>(
                input);
            output << "input_type = \"indicator\"\n";
            output << "input_indicator_id = "
                   << Quote(reference.indicator_id) << '\n';
            output << "input_output_id = "
                   << Quote(reference.output_id) << '\n';
        }
        output << "label = " << Quote(indicator.label) << '\n';
        output << "visible = "
               << (indicator.visible ? "true" : "false") << '\n';
        output << "color_rgba = "
               << static_cast<std::int64_t>(indicator.color_rgba) << '\n';
        output << "line_width = " << indicator.line_width << "\n\n";
    }
}

std::vector<ChartIndicatorState> ReadIndicators(const toml::table& table) {
    std::vector<ChartIndicatorState> indicators;
    const toml::array* values = table["indicators"].as_array();
    if (values == nullptr) return indicators;
    for (const toml::node& node : *values) {
        const toml::table* value = node.as_table();
        if (value == nullptr) continue;
        ChartIndicatorState indicator;
        indicator.definition.id = ValueOr(*value, "id", std::string{});
        indicator.definition.calculation = ReadCalculation(*value);
        const std::string input_type = ValueOr(
            *value, "input_type", std::string{"bar"});
        core::IndicatorInput input;
        if (input_type == "bar") {
            input = core::BarSeriesInput{ReadBarField(*value)};
        } else if (input_type == "indicator") {
            input = core::IndicatorOutputInput{
                .indicator_id = ValueOr(
                    *value, "input_indicator_id", std::string{}),
                .output_id = ValueOr(
                    *value, "input_output_id", std::string{"value"}),
            };
        } else {
            throw std::runtime_error(
                "Unsupported indicator input type: " + input_type);
        }
        std::visit(
            [&](auto& calculation) {
                calculation.input = std::move(input);
            },
            indicator.definition.calculation);
        indicator.label = ValueOr(*value, "label", std::string{});
        indicator.visible = ValueOr(*value, "visible", true);
        indicator.color_rgba = static_cast<std::uint32_t>(ValueOr(
            *value, "color_rgba",
            static_cast<std::int64_t>(indicator.color_rgba)));
        indicator.line_width =
            ValueOr(*value, "line_width", indicator.line_width);
        indicators.push_back(std::move(indicator));
    }
    return indicators;
}

const char* DrawingKindName(ChartDrawingKind kind) {
    switch (kind) {
        case ChartDrawingKind::HorizontalLine: return "horizontal_line";
        case ChartDrawingKind::VerticalLine: return "vertical_line";
        case ChartDrawingKind::TrendLine: return "trend_line";
        case ChartDrawingKind::Ray: return "ray";
        case ChartDrawingKind::Rectangle: return "rectangle";
    }
    return "trend_line";
}

ChartDrawingKind ReadDrawingKind(const toml::table& table) {
    const std::string value = ValueOr(
        table, "kind", std::string{"trend_line"});
    if (value == "horizontal_line") return ChartDrawingKind::HorizontalLine;
    if (value == "vertical_line") return ChartDrawingKind::VerticalLine;
    if (value == "trend_line") return ChartDrawingKind::TrendLine;
    if (value == "ray") return ChartDrawingKind::Ray;
    if (value == "rectangle") return ChartDrawingKind::Rectangle;
    throw std::runtime_error("Unsupported chart drawing kind: " + value);
}

core::Decimal ReadDecimal(const toml::table& table, std::string_view key) {
    const std::string value = ValueOr(table, key, std::string{"0"});
    const auto parsed = core::Decimal::Parse(value);
    if (!parsed)
        throw std::runtime_error(
            "Invalid chart drawing decimal: " + parsed.error().message);
    return *parsed;
}

}  // namespace

std::string EncodeProfile(const WorkstationState& state) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(3);
    output << "# Trade Box workstation profile. Credentials and market data are excluded.\n\n";
    output << "[profile]\n";
    output << "schema_version = " << state.profile.schema_version << '\n';
    output << "id = " << Quote(state.profile.id) << '\n';
    output << "name = " << Quote(state.profile.name) << "\n\n";

    output << "[application]\n";
    output << "ui_scale = " << state.application.ui_scale << '\n';
    output << "window_snap_pixels = " << state.application.window_snap_pixels << '\n';
    output << "vsync_requested = " << (state.application.vsync_requested ? "true" : "false") << '\n';
    output << "maximum_frame_rate = " << state.application.maximum_frame_rate << '\n';
      output << "account_risk_per_trade_percent = "
             << state.application.account_risk_per_trade_percent << '\n';
      output << "max_long_buying_power_percent = " << state.application.max_long_buying_power_percent << '\n';
      output << "max_short_buying_power_percent = " << state.application.max_short_buying_power_percent << '\n';
    output << "watch_list_strong_green_rgba = "
           << static_cast<std::int64_t>(state.application.watch_list_strong_green_rgba) << '\n';
    output << "watch_list_light_green_rgba = "
           << static_cast<std::int64_t>(state.application.watch_list_light_green_rgba) << '\n';
    output << "watch_list_light_red_rgba = "
           << static_cast<std::int64_t>(state.application.watch_list_light_red_rgba) << '\n';
    output << "watch_list_strong_red_rgba = "
           << static_cast<std::int64_t>(state.application.watch_list_strong_red_rgba)
           << "\n\n";

    output << "[native_window]\n";
    WriteRect(output, state.native_window.bounds);
    output << "display_id = " << Quote(state.native_window.display_id) << '\n';
    output << "maximized = " << (state.native_window.maximized ? "true" : "false") << "\n\n";

    output << "[account_context]\n";
    output << "credential_slot = " << Quote(state.account_context.credential_slot) << '\n';
    output << "account_id = " << Quote(state.account_context.account_id) << '\n';
    output << "account_alias = " << Quote(state.account_context.account_alias) << '\n';
    output << "paper = " << (state.account_context.paper ? "true" : "false") << '\n';
    output << "auto_connect = " << (state.account_context.auto_connect ? "true" : "false") << "\n\n";

    output << "[workspace]\n";
    output << "selected_symbol = " << Quote(state.workspace.selected_symbol) << '\n';
    output << "active_watch_list_id = "
           << Quote(state.workspace.active_watch_list_id) << '\n';
    output << "watchlist = ";
    WriteStringArray(output, state.workspace.watchlist);
    output << '\n';
    output << "asset_selection_history = ";
    WriteStringArray(output, state.workspace.asset_selection_history);
    output << '\n';
    output << "show_active_orders = " << (state.workspace.show_active_orders ? "true" : "false") << '\n';
    output << "show_filled_orders = " << (state.workspace.show_filled_orders ? "true" : "false") << '\n';
    output << "order_management_symbol = " << Quote(state.workspace.order_management_symbol) << '\n';
    output << "time_sales_symbol = " << Quote(state.workspace.time_sales_symbol) << '\n';
    output << "\n";

    const ChartDefaultsState& defaults = state.workspace.chart_defaults;
    output << "[chart_defaults]\n";
    output << "timeframe = " << Quote(defaults.timeframe) << '\n';
    output << "feed = " << Quote(FeedName(defaults.feed)) << '\n';
    output << "adjustment = "
           << Quote(AdjustmentName(defaults.adjustment)) << '\n';
    output << "visible_bars = " << defaults.visible_bars << '\n';
    output << "show_volume = "
           << (defaults.show_volume ? "true" : "false") << '\n';
    output << "show_close_line = "
           << (defaults.show_close_line ? "true" : "false") << '\n';
    output << "show_crosshair = "
           << (defaults.show_crosshair ? "true" : "false") << "\n\n";
    WriteIndicators(output, "chart_defaults.indicators", defaults.indicators);

    for (const auto& [symbol, draft] : state.workspace.bracket_drafts) {
        output << "[bracket_drafts." << Key(symbol) << "]\n";
        output << "target_percent = " << draft.target_percent << '\n';
        output << "stop_percent = " << draft.stop_percent << '\n';
        output << "gtc = " << (draft.gtc ? "true" : "false") << '\n';
        output << "short_entry = " << (draft.short_entry ? "true" : "false") << "\n\n";
    }

    for (const InstrumentLinkGroupState& group :
         state.workspace.instrument_link_groups) {
        output << "[[instrument_link_groups]]\n";
        output << "id = " << Quote(group.id) << '\n';
        output << "name = " << Quote(group.name) << '\n';
        output << "color = "
               << Quote(InstrumentLinkColorName(group.color)) << '\n';
        output << "assigned = "
               << (group.selected_instrument ? "true" : "false") << '\n';
        output << "instrument_id = "
               << Quote(group.selected_instrument
                            ? group.selected_instrument->instrument_id
                            : std::string{}) << '\n';
        output << "symbol = "
               << Quote(group.selected_instrument
                            ? group.selected_instrument->symbol
                            : std::string{}) << "\n\n";
    }

    for (const auto& [id, window] : state.workspace.windows) {
        output << "[windows." << Key(id) << "]\n";
        output << "id = " << Quote(window.id) << '\n';
        output << "kind = " << Quote(window.kind) << '\n';
        output << "title = " << Quote(window.title) << '\n';
        output << "open = " << (window.open ? "true" : "false") << '\n';
        WriteRect(output, window.bounds);
        output << "display_id = " << Quote(window.display_id) << '\n';
        output << "selected_tab = " << Quote(window.selected_tab) << "\n\n";
        output << "instrument_link_group_id = "
               << Quote(window.instrument_link_group_id) << "\n\n";
        for (const auto& [table_id, table] : window.tables) {
            output << "[windows." << Key(id) << ".tables." << Key(table_id) << "]\n";
            for (const ColumnState& column : table.columns) {
                output << "[[windows." << Key(id) << ".tables." << Key(table_id)
                       << ".columns]]\n";
                output << "id = " << Quote(column.id) << '\n';
                output << "order = " << column.order << '\n';
                output << "width = " << column.width << '\n';
                output << "visible = " << (column.visible ? "true" : "false") << '\n';
                output << "sort_direction = " << Quote(column.sort_direction) << "\n\n";
            }
        }
    }

    for (const WatchListDocumentState& watch_list :
         state.workspace.watch_lists) {
        output << "[[watch_lists]]\n";
        output << "id = " << Quote(watch_list.id) << '\n';
        output << "name = " << Quote(watch_list.name) << "\n\n";
        for (const WatchListRowState& row : watch_list.rows) {
            output << "[[watch_lists.rows]]\n";
            output << "id = " << Quote(row.id) << '\n';
            output << "instrument_id = " << Quote(row.instrument_id) << '\n';
            output << "symbol = " << Quote(row.symbol) << '\n';
            output << "ticker_input = " << Quote(row.ticker_input)
                   << "\n\n";
        }
    }

    output << "[time_sales_table]\n";
    for (const ColumnState& column : state.workspace.time_sales_table.columns) {
        output << "[[time_sales_table.columns]]\n";
        output << "id = " << Quote(column.id) << '\n';
        output << "order = " << column.order << '\n';
        output << "width = " << column.width << '\n';
        output << "visible = " << (column.visible ? "true" : "false") << '\n';
        output << "sort_direction = " << Quote(column.sort_direction) << "\n\n";
    }
    for (const TimeSalesDocumentState& document : state.workspace.time_sales) {
        output << "[[time_sales]]\n";
        output << "id = " << Quote(document.id) << '\n';
        output << "instrument_id = " << Quote(document.instrument_id) << '\n';
        output << "symbol = " << Quote(document.symbol) << '\n';
        output << "ticker_input = " << Quote(document.ticker_input) << "\n\n";
    }

    for (const ChartDocumentState& chart : state.workspace.charts) {
        output << "[[charts]]\n";
        output << "id = " << Quote(chart.id) << '\n';
        output << "instrument_id = " << Quote(chart.instrument_id) << '\n';
        output << "symbol = " << Quote(chart.symbol) << '\n';
        output << "ticker_input = " << Quote(chart.ticker_input) << '\n';
        output << "timeframe = " << Quote(chart.timeframe) << '\n';
        output << "feed = " << Quote(FeedName(chart.feed)) << '\n';
        output << "adjustment = "
               << Quote(AdjustmentName(chart.adjustment)) << '\n';
        output << "visible_bars = " << chart.visible_bars << '\n';
        output << "show_volume = " << (chart.show_volume ? "true" : "false") << '\n';
        output << "show_close_line = " << (chart.show_close_line ? "true" : "false") << '\n';
        output << "show_crosshair = " << (chart.show_crosshair ? "true" : "false") << '\n';
        output << "range_anchor_ns = " << chart.range_anchor_ns << "\n\n";
        WriteIndicators(output, "charts.indicators", chart.indicators);
    }

    for (const IndicatorSuiteState& suite : state.workspace.indicator_suites) {
        output << "[[indicator_suites]]\n";
        output << "id = " << Quote(suite.id) << '\n';
        output << "name = " << Quote(suite.name) << "\n\n";
        WriteIndicators(
            output, "indicator_suites.indicators", suite.indicators);
    }

    for (const ChartDrawingState& drawing : state.workspace.chart_drawings) {
        output << "[[chart_drawings]]\n";
        output << "id = " << Quote(drawing.id) << '\n';
        output << "instrument_id = " << Quote(drawing.instrument_id) << '\n';
        output << "kind = " << Quote(DrawingKindName(drawing.kind)) << '\n';
        output << "first_time_ns = " << drawing.first.time_ns << '\n';
        output << "first_price = " << Quote(drawing.first.price.ToString()) << '\n';
        output << "has_second = "
               << (drawing.second ? "true" : "false") << '\n';
        output << "second_time_ns = "
               << (drawing.second ? drawing.second->time_ns : 0) << '\n';
        output << "second_price = "
               << Quote(drawing.second
                            ? drawing.second->price.ToString()
                            : std::string{"0"}) << '\n';
        output << "label = " << Quote(drawing.label) << '\n';
        output << "visible = "
               << (drawing.visible ? "true" : "false") << '\n';
        output << "color_rgba = "
               << static_cast<std::int64_t>(drawing.color_rgba) << '\n';
        output << "line_width = " << drawing.line_width << "\n\n";
    }

    return output.str();
}

std::expected<WorkstationState, std::string> DecodeProfile(std::string_view source) {
    try {
        const toml::table root = toml::parse(source);
        WorkstationState state = WorkstationState::Defaults();
        if (const toml::table* profile = root["profile"].as_table()) {
            state.profile.schema_version = ValueOr(*profile, "schema_version", 0);
            state.profile.id = ValueOr(*profile, "id", std::string{});
            state.profile.name = ValueOr(*profile, "name", state.profile.name);
        }
        const int source_schema_version = state.profile.schema_version;
        if (source_schema_version != 1 && source_schema_version != 2 &&
            source_schema_version != 3 && source_schema_version != 4 &&
            source_schema_version != 5 && source_schema_version != 6 &&
            source_schema_version != 7 &&
            source_schema_version != kCurrentSchemaVersion)
            return std::unexpected(
                "Unsupported workstation profile schema version");
        state.profile.schema_version = kCurrentSchemaVersion;
        if (const toml::table* application = root["application"].as_table()) {
            state.application.ui_scale = ValueOr(*application, "ui_scale", state.application.ui_scale);
            state.application.window_snap_pixels = ValueOr(*application, "window_snap_pixels", state.application.window_snap_pixels);
            state.application.vsync_requested = ValueOr(*application, "vsync_requested", state.application.vsync_requested);
            state.application.maximum_frame_rate = ValueOr(*application, "maximum_frame_rate", state.application.maximum_frame_rate);
              state.application.account_risk_per_trade_percent = ValueOr(
                  *application, "account_risk_per_trade_percent",
                  state.application.account_risk_per_trade_percent);
              state.application.max_long_buying_power_percent = ValueOr(*application, "max_long_buying_power_percent", state.application.max_long_buying_power_percent);
              state.application.max_short_buying_power_percent = ValueOr(*application, "max_short_buying_power_percent", state.application.max_short_buying_power_percent);
            state.application.watch_list_strong_green_rgba = static_cast<std::uint32_t>(
                ValueOr(*application, "watch_list_strong_green_rgba",
                        static_cast<std::int64_t>(state.application.watch_list_strong_green_rgba)));
            state.application.watch_list_light_green_rgba = static_cast<std::uint32_t>(
                ValueOr(*application, "watch_list_light_green_rgba",
                        static_cast<std::int64_t>(state.application.watch_list_light_green_rgba)));
            state.application.watch_list_light_red_rgba = static_cast<std::uint32_t>(
                ValueOr(*application, "watch_list_light_red_rgba",
                        static_cast<std::int64_t>(state.application.watch_list_light_red_rgba)));
            state.application.watch_list_strong_red_rgba = static_cast<std::uint32_t>(
                ValueOr(*application, "watch_list_strong_red_rgba",
                        static_cast<std::int64_t>(state.application.watch_list_strong_red_rgba)));
        }
        if (const toml::table* native = root["native_window"].as_table()) {
            state.native_window.bounds = ReadRect(*native, state.native_window.bounds);
            state.native_window.display_id = ValueOr(*native, "display_id", std::string{});
            state.native_window.maximized = ValueOr(*native, "maximized", false);
        }
        if (const toml::table* account = root["account_context"].as_table()) {
            state.account_context.credential_slot = ValueOr(*account, "credential_slot", std::string{});
            state.account_context.account_id = ValueOr(*account, "account_id", std::string{});
            state.account_context.account_alias = ValueOr(*account, "account_alias", std::string{});
            state.account_context.paper = ValueOr(*account, "paper", true);
            state.account_context.auto_connect = ValueOr(*account, "auto_connect", false);
        }
        if (const toml::table* workspace = root["workspace"].as_table()) {
            state.workspace.selected_symbol = ValueOr(*workspace, "selected_symbol", state.workspace.selected_symbol);
            state.workspace.active_watch_list_id = ValueOr(
                *workspace, "active_watch_list_id", std::string{});
            state.workspace.watchlist = ReadStringArray(*workspace, "watchlist");
            state.workspace.asset_selection_history =
                ReadStringArray(*workspace, "asset_selection_history");
            state.workspace.show_active_orders = ValueOr(*workspace, "show_active_orders", true);
            state.workspace.show_filled_orders = ValueOr(*workspace, "show_filled_orders", true);
            state.workspace.order_management_symbol = ValueOr(*workspace, "order_management_symbol", std::string{});
            state.workspace.time_sales_symbol = ValueOr(*workspace, "time_sales_symbol", state.workspace.time_sales_symbol);
        }
        if (const toml::table* defaults = root["chart_defaults"].as_table()) {
            state.workspace.chart_defaults.timeframe = ValueOr(
                *defaults, "timeframe",
                state.workspace.chart_defaults.timeframe);
            state.workspace.chart_defaults.feed = ReadFeed(
                *defaults, state.workspace.chart_defaults.feed);
            state.workspace.chart_defaults.adjustment = ReadAdjustment(
                *defaults, state.workspace.chart_defaults.adjustment);
            state.workspace.chart_defaults.visible_bars = ValueOr(
                *defaults, "visible_bars",
                state.workspace.chart_defaults.visible_bars);
            state.workspace.chart_defaults.show_volume = ValueOr(
                *defaults, "show_volume",
                state.workspace.chart_defaults.show_volume);
            state.workspace.chart_defaults.show_close_line = ValueOr(
                *defaults, "show_close_line",
                state.workspace.chart_defaults.show_close_line);
            state.workspace.chart_defaults.show_crosshair = ValueOr(
                *defaults, "show_crosshair",
                state.workspace.chart_defaults.show_crosshair);
            state.workspace.chart_defaults.indicators =
                ReadIndicators(*defaults);
        }
        state.workspace.bracket_drafts.clear();
        if (const toml::table* drafts = root["bracket_drafts"].as_table()) {
            for (const auto& [key, node] : *drafts) {
                const toml::table* draft_table = node.as_table();
                if (draft_table == nullptr) continue;
                BracketDraftState draft;
                draft.target_percent = ValueOr(*draft_table, "target_percent", draft.target_percent);
                draft.stop_percent = ValueOr(*draft_table, "stop_percent", draft.stop_percent);
                draft.gtc = ValueOr(*draft_table, "gtc", draft.gtc);
                draft.short_entry = ValueOr(*draft_table, "short_entry", draft.short_entry);
                state.workspace.bracket_drafts.emplace(std::string(key.str()), draft);
            }
        }
        if (const toml::array* groups =
                root["instrument_link_groups"].as_array()) {
            if (groups->size() != kInstrumentLinkGroupCount)
                return std::unexpected(
                    "Instrument link profile must contain exactly 32 groups");
            std::size_t index = 0;
            for (const toml::node& node : *groups) {
                const toml::table* group = node.as_table();
                if (group == nullptr)
                    return std::unexpected(
                        "Instrument link profile contains an invalid group");
                InstrumentLinkGroupState value{
                    .id = ValueOr(*group, "id", std::string{}),
                    .name = ValueOr(*group, "name", std::string{}),
                    .color = ReadInstrumentLinkColor(*group),
                };
                if (ValueOr(*group, "assigned", false)) {
                    value.selected_instrument = InstrumentSelectionState{
                        .instrument_id = ValueOr(
                            *group, "instrument_id", std::string{}),
                        .symbol = ValueOr(
                            *group, "symbol", std::string{}),
                    };
                }
                state.workspace.instrument_link_groups[index++] =
                    std::move(value);
            }
        }
        state.workspace.windows.clear();
        if (const toml::table* windows = root["windows"].as_table()) {
            for (const auto& [key, node] : *windows) {
                const toml::table* window_table = node.as_table();
                if (window_table == nullptr) continue;
                const std::string id(key.str());
                WindowInstanceState window;
                window.id = ValueOr(*window_table, "id", id);
                window.kind = ValueOr(*window_table, "kind", std::string{"tool"});
                window.title = ValueOr(*window_table, "title", id);
                window.open = ValueOr(*window_table, "open", true);
                window.bounds = ReadRect(*window_table, {24, 24, 420, 280});
                window.display_id = ValueOr(*window_table, "display_id", std::string{});
                window.selected_tab = ValueOr(*window_table, "selected_tab", std::string{});
                window.instrument_link_group_id = ValueOr(
                    *window_table, "instrument_link_group_id", std::string{});
                if (const toml::table* tables = (*window_table)["tables"].as_table()) {
                    for (const auto& [table_key, table_node] : *tables) {
                        const toml::table* table_value = table_node.as_table();
                        if (table_value == nullptr) continue;
                        PersistentTableState table;
                        if (const toml::array* columns = (*table_value)["columns"].as_array()) {
                            for (const toml::node& column_node : *columns) {
                                const toml::table* column_value = column_node.as_table();
                                if (column_value == nullptr) continue;
                                table.columns.push_back({
                                    .id = ValueOr(*column_value, "id", std::string{}),
                                    .order = ValueOr(*column_value, "order", 0),
                                    .width = ValueOr(*column_value, "width", 0.0f),
                                    .visible = ValueOr(*column_value, "visible", true),
                                    .sort_direction = ValueOr(*column_value, "sort_direction", std::string{}),
                                });
                            }
                        }
                        window.tables.emplace(std::string(table_key.str()), std::move(table));
                    }
                }
                state.workspace.windows.emplace(id, std::move(window));
            }
        }
        state.workspace.watch_lists.clear();
        if (const toml::array* watch_lists = root["watch_lists"].as_array()) {
            for (const toml::node& node : *watch_lists) {
                const toml::table* watch_list = node.as_table();
                if (watch_list == nullptr) continue;
                WatchListDocumentState document{
                    .id = ValueOr(*watch_list, "id", std::string{}),
                    .name = ValueOr(*watch_list, "name",
                                    std::string{"Watch List"}),
                };
                if (const toml::array* rows =
                        (*watch_list)["rows"].as_array()) {
                    for (const toml::node& row_node : *rows) {
                        const toml::table* row = row_node.as_table();
                        if (row == nullptr) continue;
                        document.rows.push_back({
                            .id = ValueOr(*row, "id", std::string{}),
                            .instrument_id = ValueOr(
                                *row, "instrument_id", std::string{}),
                            .symbol = ValueOr(
                                *row, "symbol", std::string{}),
                            .ticker_input = ValueOr(
                                *row, "ticker_input", std::string{}),
                        });
                    }
                }
                state.workspace.watch_lists.push_back(std::move(document));
            }
        }
        state.workspace.time_sales.clear();
        if (const toml::array* documents = root["time_sales"].as_array()) {
            for (const toml::node& node : *documents) {
                const toml::table* document = node.as_table();
                if (document == nullptr) continue;
                state.workspace.time_sales.push_back({
                    .id = ValueOr(*document, "id", std::string{}),
                    .instrument_id = ValueOr(*document, "instrument_id", std::string{}),
                    .symbol = ValueOr(*document, "symbol", std::string{}),
                    .ticker_input = ValueOr(*document, "ticker_input", std::string{}),
                });
            }
        }
        state.workspace.time_sales_table.columns.clear();
        if (const toml::table* table = root["time_sales_table"].as_table()) {
            if (const toml::array* columns = (*table)["columns"].as_array()) {
                for (const toml::node& node : *columns) {
                    const toml::table* column = node.as_table();
                    if (column == nullptr) continue;
                    state.workspace.time_sales_table.columns.push_back({
                        .id = ValueOr(*column, "id", std::string{}),
                        .order = ValueOr(*column, "order", 0),
                        .width = ValueOr(*column, "width", 0.0f),
                        .visible = ValueOr(*column, "visible", true),
                        .sort_direction = ValueOr(*column, "sort_direction", std::string{}),
                    });
                }
            }
        }
        state.workspace.charts.clear();
        if (const toml::array* charts = root["charts"].as_array()) {
            for (const toml::node& node : *charts) {
                const toml::table* chart = node.as_table();
                if (chart == nullptr) continue;
                ChartDocumentState document{
                    .id = ValueOr(*chart, "id", std::string{}),
                    .instrument_id = ValueOr(*chart, "instrument_id", std::string{}),
                    .symbol = ValueOr(*chart, "symbol", std::string{}),
                    .ticker_input = ValueOr(*chart, "ticker_input", std::string{}),
                    .timeframe = ValueOr(*chart, "timeframe", std::string{"1Min"}),
                    .feed = ReadFeed(*chart, core::MarketDataFeed::Iex),
                    .adjustment = ReadAdjustment(*chart, core::BarAdjustment::Raw),
                    .visible_bars = ValueOr(*chart, "visible_bars", 120),
                    .show_volume = ValueOr(*chart, "show_volume", true),
                    .show_close_line = ValueOr(*chart, "show_close_line", false),
                    .show_crosshair = ValueOr(*chart, "show_crosshair", true),
                    .range_anchor_ns = ValueOr(
                        *chart, "range_anchor_ns", std::int64_t{0}),
                };
                document.indicators = ReadIndicators(*chart);
                state.workspace.charts.push_back(std::move(document));
            }
        }
        state.workspace.indicator_suites.clear();
        if (const toml::array* suites = root["indicator_suites"].as_array()) {
            for (const toml::node& node : *suites) {
                const toml::table* suite = node.as_table();
                if (suite == nullptr) continue;
                state.workspace.indicator_suites.push_back({
                    .id = ValueOr(*suite, "id", std::string{}),
                    .name = ValueOr(*suite, "name", std::string{}),
                    .indicators = ReadIndicators(*suite),
                });
            }
        }
        state.workspace.chart_drawings.clear();
        if (const toml::array* drawings = root["chart_drawings"].as_array()) {
            for (const toml::node& node : *drawings) {
                const toml::table* drawing = node.as_table();
                if (drawing == nullptr) continue;
                ChartDrawingState value{
                    .id = ValueOr(*drawing, "id", std::string{}),
                    .instrument_id = ValueOr(
                        *drawing, "instrument_id", std::string{}),
                    .kind = ReadDrawingKind(*drawing),
                    .first = {
                        .time_ns = ValueOr(
                            *drawing, "first_time_ns", std::int64_t{0}),
                        .price = ReadDecimal(*drawing, "first_price"),
                    },
                    .label = ValueOr(*drawing, "label", std::string{}),
                    .visible = ValueOr(*drawing, "visible", true),
                    .color_rgba = static_cast<std::uint32_t>(ValueOr(
                        *drawing, "color_rgba",
                        static_cast<std::int64_t>(0xd8d8d8ffU))),
                    .line_width = ValueOr(*drawing, "line_width", 1.5f),
                };
                if (ValueOr(*drawing, "has_second", false)) {
                    value.second = ChartDrawingAnchorState{
                        .time_ns = ValueOr(
                            *drawing, "second_time_ns", std::int64_t{0}),
                        .price = ReadDecimal(*drawing, "second_price"),
                    };
                }
                state.workspace.chart_drawings.push_back(std::move(value));
            }
        }
        if (source_schema_version == 7)
            RemoveSchemaSevenWatchListSeed(state.workspace);
        std::string validation_error;
        if (!ValidateAndNormalize(state, validation_error))
            return std::unexpected(validation_error);
        return state;
    } catch (const toml::parse_error& error) {
        return std::unexpected(std::string("Invalid workstation profile: ") +
                               std::string(error.description()));
    } catch (const std::exception& error) {
        return std::unexpected(std::string("Could not decode workstation profile: ") + error.what());
    }
}

}  // namespace tradebox::workstation
