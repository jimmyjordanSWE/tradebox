#pragma once

#include "tradebox/core/indicator.h"

#include <cstdint>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tradebox::workstation {

inline constexpr int kCurrentSchemaVersion = 3;
inline constexpr std::size_t kInstrumentLinkGroupCount = 32;

enum class InstrumentLinkColor : std::uint8_t {
    Red,
    Crimson,
    Orange,
    Amber,
    Yellow,
    Lime,
    Green,
    Emerald,
    Teal,
    Cyan,
    Sky,
    Blue,
    Indigo,
    Violet,
    Purple,
    Magenta,
    Rose,
    Coral,
    Peach,
    Gold,
    Olive,
    Mint,
    Aqua,
    Navy,
    Lavender,
    Plum,
    Maroon,
    Brown,
    Slate,
    Gray,
    Black,
    White,
};

struct InstrumentSelectionState {
    std::string instrument_id;
    std::string symbol;

    bool operator==(const InstrumentSelectionState&) const = default;
};

struct InstrumentLinkGroupState {
    std::string id;
    std::string name;
    InstrumentLinkColor color = InstrumentLinkColor::Red;
    std::optional<InstrumentSelectionState> selected_instrument;
};

struct LogicalRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct NativeWindowState {
    LogicalRect bounds{120.0f, 80.0f, 1920.0f, 1080.0f};
    std::string display_id;
    bool maximized = false;
};

struct ApplicationSettings {
    float ui_scale = 1.0f;
    int window_snap_pixels = 10;
    bool vsync_requested = true;
    int maximum_frame_rate = 120;
};

struct AccountContext {
    std::string credential_slot;
    std::string account_id;
    std::string account_alias;
    bool paper = true;
    bool auto_connect = false;
};

struct ColumnState {
    std::string id;
    int order = 0;
    float width = 0.0f;
    bool visible = true;
    std::string sort_direction;
};

struct PersistentTableState {
    std::vector<ColumnState> columns;
};

struct WatchListRowState {
    std::string id;
    std::string instrument_id;
    std::string symbol;
    std::string ticker_input;
};

struct WatchListDocumentState {
    std::string id;
    std::string name = "Watch List";
    std::vector<WatchListRowState> rows;
};

struct WindowInstanceState {
    std::string id;
    std::string kind;
    std::string title;
    bool open = true;
    LogicalRect bounds{};
    std::string display_id;
    std::string selected_tab;
    std::string instrument_link_group_id;
    std::map<std::string, PersistentTableState, std::less<>> tables;
};

struct ChartIndicatorState {
    core::IndicatorDefinition definition;
    std::string label;
    bool visible = true;
    std::uint32_t color_rgba = 0x4aa3d8ffU;
    float line_width = 1.5f;

    bool operator==(const ChartIndicatorState&) const = default;
};

struct ChartDefaultsState {
    std::string timeframe = "1Min";
    core::MarketDataFeed feed = core::MarketDataFeed::Iex;
    core::BarAdjustment adjustment = core::BarAdjustment::Raw;
    int visible_bars = 120;
    bool show_volume = true;
    bool show_close_line = false;
    bool show_crosshair = true;
    std::vector<ChartIndicatorState> indicators;
};

struct ChartDocumentState {
    std::string id;
    std::string instrument_id;
    std::string symbol;
    // Editable ticker text is persisted independently from a resolved asset.
    // It may be partial while instrument_id and symbol remain empty.
    std::string ticker_input;
    std::string timeframe = "1Min";
    core::MarketDataFeed feed = core::MarketDataFeed::Iex;
    core::BarAdjustment adjustment = core::BarAdjustment::Raw;
    int visible_bars = 120;
    bool show_volume = true;
    bool show_close_line = false;
    bool show_crosshair = true;
    // Zero means follow the latest available market-data anchor. A non-zero
    // value is the persisted viewport anchor selected by the user.
    std::int64_t range_anchor_ns = 0;
    std::vector<ChartIndicatorState> indicators;
};

struct IndicatorSuiteState {
    std::string id;
    std::string name;
    std::vector<ChartIndicatorState> indicators;
};

enum class ChartDrawingKind {
    HorizontalLine,
    VerticalLine,
    TrendLine,
    Ray,
    Rectangle,
};

struct ChartDrawingAnchorState {
    std::int64_t time_ns = 0;
    core::Decimal price;

    bool operator==(const ChartDrawingAnchorState&) const = default;
};

struct ChartDrawingState {
    std::string id;
    // Drawings are shared by every chart displaying this stable instrument.
    std::string instrument_id;
    ChartDrawingKind kind = ChartDrawingKind::TrendLine;
    ChartDrawingAnchorState first;
    std::optional<ChartDrawingAnchorState> second;
    std::string label;
    bool visible = true;
    std::uint32_t color_rgba = 0xd8d8d8ffU;
    float line_width = 1.5f;
};

struct OrderTicketState {
    std::string id;
    bool open = true;
    std::string name = "Untitled order";
    std::string symbol = "AMD";
    std::string side = "buy";
    std::string amount = "1";
    bool amount_is_notional = false;
    std::string type = "market";
    std::string limit_price;
    std::string stop_price;
    std::string time_in_force = "day";
    bool extended_hours = false;
    std::string credential_slot;
    std::string account_id;
    bool paper = true;
};

struct BracketDraftState {
    float target_percent = 1.0f;
    float stop_percent = 0.5f;
    bool gtc = true;
    bool short_entry = false;
};

struct WorkspaceState {
    std::string selected_symbol = "AMD";
    std::vector<std::string> watchlist{"AMD", "AAPL", "NVDA", "SPY"};
    // Most-recently selected stable asset identities, used to make repeated
    // autocomplete choices appear before the alphabetical universe.
    std::vector<std::string> asset_selection_history;
    bool show_active_orders = true;
    bool show_filled_orders = true;
    std::string order_management_symbol;
    std::string time_sales_symbol = "AMD";
    float quick_long_buying_power_percent = 100.0f;
    float quick_short_buying_power_percent = 80.0f;
    std::map<std::string, BracketDraftState, std::less<>> bracket_drafts;
    std::array<InstrumentLinkGroupState, kInstrumentLinkGroupCount>
        instrument_link_groups;
    std::map<std::string, WindowInstanceState, std::less<>> windows;
    ChartDefaultsState chart_defaults;
    std::vector<ChartDocumentState> charts;
    std::vector<WatchListDocumentState> watch_lists;
    std::vector<IndicatorSuiteState> indicator_suites;
    std::vector<ChartDrawingState> chart_drawings;
    std::vector<OrderTicketState> order_tickets;
};

struct ProfileMetadata {
    int schema_version = kCurrentSchemaVersion;
    std::string id;
    std::string name = "Default";
};

struct WorkstationState {
    ProfileMetadata profile;
    ApplicationSettings application;
    NativeWindowState native_window;
    AccountContext account_context;
    WorkspaceState workspace;

    [[nodiscard]] static WorkstationState Defaults();
};

}  // namespace tradebox::workstation
