#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tradebox::workstation {

inline constexpr int kCurrentSchemaVersion = 1;

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

struct WindowInstanceState {
    std::string id;
    std::string kind;
    std::string title;
    bool open = true;
    LogicalRect bounds{};
    std::string display_id;
    std::string selected_tab;
    std::map<std::string, PersistentTableState, std::less<>> tables;
};

struct ChartDocumentState {
    std::string id;
    std::string instrument_id;
    std::string symbol;
    std::string timeframe = "1Min";
    int visible_bars = 120;
    bool show_volume = true;
    bool show_close_line = false;
    bool show_crosshair = true;
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
    bool show_active_orders = true;
    bool show_filled_orders = true;
    std::string order_management_symbol;
    std::string time_sales_symbol = "AMD";
    float quick_long_buying_power_percent = 100.0f;
    float quick_short_buying_power_percent = 80.0f;
    std::map<std::string, BracketDraftState, std::less<>> bracket_drafts;
    std::map<std::string, WindowInstanceState, std::less<>> windows;
    std::vector<ChartDocumentState> charts;
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

