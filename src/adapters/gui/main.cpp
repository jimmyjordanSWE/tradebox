#include "tradebox/application/trading_application.h"
#include "tradebox/core/order_projection.h"
#include "tradebox/persistence/database.h"
#include "tradebox/platform/credentials.h"
#include "tradebox/ui/chart_view.h"
#include "tradebox/ui/model.h"
#include "tradebox/ui/workspace.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <d3d11.h>
#include <windows.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_sdl3.h"
#include "implot.h"

namespace {

using Microsoft::WRL::ComPtr;

struct Dx11Renderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11RenderTargetView> render_target;
    int width = 0;
    int height = 0;
};
enum class ConnectionState {
    Disconnected,
    Connecting,
    AccountAuthenticated,
    Streaming,
    Error
};

enum class EventSeverity {
    Auto,
    Info,
    Warning,
    Error,
};

struct EventLogEntry {
    std::int64_t recorded_at_ms = 0;
    EventSeverity severity = EventSeverity::Info;
    std::string text;
};

struct LaunchOptions {
    std::filesystem::path capture_path;
    bool capture_and_exit = false;
    bool connect_paper = false;
    int run_for_ms = 0;
};

struct Chart {
    std::string symbol;
    std::string timeframe = "1Min";
    tradebox::core::BarSeriesSnapshot core_snapshot;
    tradebox::core::BarRange requested_range;
    std::vector<Bar> bars;
    tradebox::ui::ChartViewSeries view_series;
    tradebox::ui::ChartViewDataState view_state =
        tradebox::ui::ChartViewDataState::Loading;
    tradebox::ui::ChartViewOptions view_options;
    int visible_bars = 120;
    bool reset_x_range = true;
    std::string data_message;
    bool history_loaded = false;
    bool window_open = false;
};

struct App {
    explicit App(Database& db)
        : database(db),
          application(events, database) {}

    Database& database;
    UiEventQueue events;
    tradebox::application::TradingApplication application;
    std::vector<std::string> watchlist;
    tradebox::ui::Workspace workspace;
    tradebox::ui::UiScaleController ui_scale;
    std::vector<std::string> requested_market_symbols;
    tradebox::application::ApplicationUiSnapshot ui_snapshot;
    std::unordered_map<std::string, Chart> charts;
    std::unordered_map<std::string, tradebox::core::MarketDataSnapshot>
        market_views;
    // Read-only projections copied from the headless core for rendering.
    tradebox::core::CoreSnapshot core_view;
    tradebox::core::AccountState account;
    bool has_account = false;
    std::string account_alias;
    std::array<char, 96> account_alias_edit{};
    std::vector<tradebox::core::PositionState> positions;
    std::vector<tradebox::core::OrderState> orders;
    std::int64_t positions_received_at_ms = 0;
    std::int64_t orders_received_at_ms = 0;
    MarketClockSnapshot market_clock;
    bool has_market_clock = false;
    bool active_paper = true;
    bool credentials_open = false;
    bool show_account = true;
    bool show_watchlist = true;
    bool show_activity = true;
    bool show_active_orders = true;
    bool show_filled_orders = true;
    bool show_time_sales = true;
    bool show_quick_order = true;
    bool show_oco_order = true;
    bool show_order_management = true;
    ConnectionState connection_state = ConnectionState::Disconnected;
    std::int64_t market_latency_ms = -1;
    std::int64_t last_market_received_at_ms = 0;
    bool market_subscription_active = false;
    std::int64_t account_stream_latency_ms = -1;
    std::int64_t last_account_stream_event_at_ms = 0;
    std::int64_t account_event_latency_ms = -1;
    bool account_stream_failed = false;
    bool credential_paper = true;
    std::array<char, 160> key{};
    std::array<char, 160> secret{};
    std::array<char, 24> new_symbol{};
    std::vector<EventLogEntry> messages;
    struct OrderTicket {
        OrderEntryDraft draft;
        std::array<char, 96> name{'U','n','t','i','t','l','e','d',' ','o','r','d','e','r'};
        std::array<char, 24> symbol{'A','M','D'};
        std::array<char, 64> amount{'1'};
        std::array<char, 64> limit{};
        std::array<char, 64> stop{};
        std::string symbol_query;
        std::vector<tradebox::core::TradableAsset> symbol_matches;
        int symbol_cursor = 0;
        bool window_open = true;
        bool was_visible = false;
    };
    std::vector<OrderTicket> order_tickets;
    std::vector<tradebox::core::TradableAsset> asset_catalog;
    std::array<char, 96> asset_query{};
    int asset_cursor = 0;
    std::vector<tradebox::core::TradableAsset> asset_matches;
    std::string watchlist_asset_query;
    std::string selected_symbol = "AMD";
    std::array<char, 24> time_sales_symbol{'A','M','D'};
    std::string time_sales_query;
    std::vector<tradebox::core::TradableAsset> time_sales_matches;
    int time_sales_cursor = 0;
    float quick_long_buying_power_percent = 100.0f;
    float quick_short_buying_power_percent = 80.0f;
    std::array<char, 24> order_management_symbol{};
    struct OcoDraft {
        float target_percent = 1.0f;
        float stop_percent = 0.5f;
        bool gtc = true;
        bool short_entry = false;
    };
    std::unordered_map<std::string, OcoDraft> oco_drafts;
    std::size_t linked_order_ticket = 0;
    bool show_order_entry = true;
    bool command_in_flight = false;
    std::string command_request_id;
    std::string command_status;
    std::unordered_map<std::string, std::string> command_sources;
    std::unordered_map<std::string, std::string> command_status_by_source;
    std::uint64_t next_request_id = 1;
    std::vector<std::jthread> command_workers;
    bool capture_requested = false;
    std::filesystem::path capture_path;
    bool exit_after_capture = false;
    int frames_before_capture = 0;
    bool vsync_requested = true;
    bool vsync_enabled = false;
    float display_refresh_hz = 0;
    SDL_DisplayID presentation_display = 0;
    int presentation_rearm_frames = 0;
    int maximum_frame_rate = 120;
    bool software_frame_pacing = false;
    Uint64 next_frame_deadline_ns = 0;
    MarketDataStorageUsage market_storage;
    Uint64 market_storage_sampled_at = 0;
    DatabaseWriterTelemetry writer_telemetry;
    Uint64 writer_telemetry_sampled_at = 0;
};

bool LoadWindowVisible(App& app, const char* key, bool fallback) {
    const auto value = app.database.LoadAppSetting(key);
    if (!value) return fallback;
    return *value == "1";
}

void LoadWindowVisibility(App& app) {
    app.show_account = LoadWindowVisible(app, "ui.window.account", true);
    app.show_watchlist = LoadWindowVisible(app, "ui.window.watchlist", true);
    app.show_activity = LoadWindowVisible(app, "ui.window.activity", true);
    app.show_time_sales =
        LoadWindowVisible(app, "ui.window.time_sales", true);
    app.show_quick_order =
        LoadWindowVisible(app, "ui.window.quick_order", true);
    app.show_oco_order =
        LoadWindowVisible(app, "ui.window.oco_order", true);
    app.show_order_management =
        LoadWindowVisible(app, "ui.window.order_management", true);
    app.show_order_entry =
        LoadWindowVisible(app, "ui.window.order_entry", true);
    if (const auto value = app.database.LoadAppSetting(
            "ui.quick_order.long_buying_power_percent"))
        app.quick_long_buying_power_percent = std::clamp(
            std::strtof(value->c_str(), nullptr), 1.0f, 100.0f);
    if (const auto value = app.database.LoadAppSetting(
            "ui.quick_order.short_buying_power_percent"))
        app.quick_short_buying_power_percent = std::clamp(
            std::strtof(value->c_str(), nullptr), 1.0f, 100.0f);
}

void SaveWindowVisibility(const App& app) {
    const auto save = [&app](const char* key, bool visible) {
        app.database.SaveAppSetting(key, visible ? "1" : "0");
    };
    save("ui.window.account", app.show_account);
    save("ui.window.watchlist", app.show_watchlist);
    save("ui.window.activity", app.show_activity);
    save("ui.window.time_sales", app.show_time_sales);
    save("ui.window.quick_order", app.show_quick_order);
    save("ui.window.oco_order", app.show_oco_order);
    save("ui.window.order_management", app.show_order_management);
    save("ui.window.order_entry", app.show_order_entry);
    app.database.SaveAppSetting(
        "ui.quick_order.long_buying_power_percent",
        std::to_string(app.quick_long_buying_power_percent));
    app.database.SaveAppSetting(
        "ui.quick_order.short_buying_power_percent",
        std::to_string(app.quick_short_buying_power_percent));

    std::string open_charts;
    for (const auto& [symbol, chart] : app.charts) {
        if (!chart.window_open) continue;
        if (!open_charts.empty()) open_charts += ',';
        open_charts += symbol;
    }
    app.database.SaveAppSetting("ui.window.open_charts", open_charts);
}

void RestoreOpenCharts(App& app) {
    const auto saved = app.database.LoadAppSetting("ui.window.open_charts");
    if (!saved || saved->empty()) return;

    std::size_t start = 0;
    while (start < saved->size()) {
        const std::size_t comma = saved->find(',', start);
        const std::string symbol = saved->substr(start, comma - start);
        if (auto chart = app.charts.find(symbol); chart != app.charts.end())
            chart->second.window_open = true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

void RequestChartData(App& app, Chart& chart);

std::int64_t SystemNowMs();

bool ContainsInsensitive(std::string_view text, std::string_view needle) {
    return std::search(
               text.begin(), text.end(), needle.begin(), needle.end(),
               [](char left, char right) {
                   return std::tolower(static_cast<unsigned char>(left)) ==
                          std::tolower(static_cast<unsigned char>(right));
               }) != text.end();
}

EventSeverity ClassifyEventSeverity(std::string_view message) {
    for (const std::string_view marker :
         {"error", "failed", "rejected", "could not", "indeterminate"}) {
        if (ContainsInsensitive(message, marker)) return EventSeverity::Error;
    }
    for (const std::string_view marker :
         {"warning", "stale", "waiting", "unavailable", "reconnecting"}) {
        if (ContainsInsensitive(message, marker)) return EventSeverity::Warning;
    }
    return EventSeverity::Info;
}

EventSeverity EventSeverityFromOperational(OperationalSeverity severity) {
    switch (severity) {
    case OperationalSeverity::Critical: return EventSeverity::Error;
    case OperationalSeverity::Warning: return EventSeverity::Warning;
    case OperationalSeverity::Informational: return EventSeverity::Info;
    }
    return EventSeverity::Info;
}

void AddMessage(App& app, std::string message,
                EventSeverity severity = EventSeverity::Auto) {
    if (message.empty()) return;
    if (severity == EventSeverity::Auto)
        severity = ClassifyEventSeverity(message);
    app.messages.insert(app.messages.begin(), EventLogEntry{
        .recorded_at_ms = SystemNowMs(),
        .severity = severity,
        .text = std::move(message),
    });
    if (app.messages.size() > 100) app.messages.resize(100);
}

void ClearCredentialInputs(App& app) {
    SecureZeroMemory(app.key.data(), app.key.size());
    SecureZeroMemory(app.secret.data(), app.secret.size());
}

void ApplyPresentationSettings(App& app, SDL_Window* window, bool announce) {
    app.software_frame_pacing = app.maximum_frame_rate > 0;
    app.next_frame_deadline_ns = 0;
    // DirectX selects the presentation interval at Present(). Unlike WGL,
    // there is no mutable per-window swap-interval state to rebind here.
    app.vsync_enabled = app.vsync_requested;
    static_cast<void>(window);
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    app.presentation_display = display;
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    app.display_refresh_hz = mode ? mode->refresh_rate : 0;
    if (announce) {
        std::ostringstream message;
        if (app.vsync_enabled) {
            message << "VSync active (DirectX)";
        } else if (app.vsync_requested) {
            message << "VSync requested but unavailable";
        } else {
            message << "VSync off";
        }
        if (app.maximum_frame_rate > 0)
            message << "; capped at " << app.maximum_frame_rate << " FPS";
        else
            message << "; frame rate uncapped";
        if (app.display_refresh_hz > 0)
            message << " on " << std::fixed << std::setprecision(0)
                    << app.display_refresh_hz << " Hz display";
        AddMessage(app, message.str());
    }
}

void PaceSoftwareFrame(App& app) {
    if (!app.software_frame_pacing || app.maximum_frame_rate <= 0) return;
    const Uint64 period_ns = static_cast<Uint64>(
        1000000000.0 / static_cast<double>(app.maximum_frame_rate));
    const Uint64 now = SDL_GetTicksNS();
    if (app.next_frame_deadline_ns == 0 ||
        now > app.next_frame_deadline_ns + period_ns * 4)
        app.next_frame_deadline_ns = now + period_ns;
    else
        app.next_frame_deadline_ns += period_ns;
    const Uint64 before_wait = SDL_GetTicksNS();
    if (before_wait < app.next_frame_deadline_ns)
        SDL_DelayPrecise(app.next_frame_deadline_ns - before_wait);
}

std::string NormalizeSymbol(const char* input) {
    std::string symbol;
    for (const unsigned char character : std::string(input)) {
        if (std::isalnum(character) || character == '.' || character == '-')
            symbol.push_back(static_cast<char>(std::toupper(character)));
    }
    return symbol;
}

bool PlacementTouchesDisplay(const WindowPlacement& placement) {
    const SDL_Rect window_rect{placement.x, placement.y, placement.width,
                               placement.height};
    int display_count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
    if (!displays) return false;
    for (int index = 0; index < display_count; ++index) {
        SDL_Rect usable{};
        if (!SDL_GetDisplayUsableBounds(displays[index], &usable)) continue;
        SDL_Rect intersection{};
        if (SDL_GetRectIntersection(&window_rect, &usable, &intersection) &&
            intersection.w >= 100 && intersection.h >= 100) {
            SDL_free(displays);
            return true;
        }
    }
    SDL_free(displays);
    return false;
}

std::int64_t SystemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string FormatDelay(std::int64_t milliseconds);

std::filesystem::path ApplicationAssetPath(
    const std::filesystem::path& relative_path) {
    std::array<wchar_t, 32'768> executable_path{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executable_path.data(),
        static_cast<DWORD>(executable_path.size()));
    if (length == 0 ||
        length >= static_cast<DWORD>(executable_path.size()))
        return {};
    return std::filesystem::path(executable_path.data()).parent_path() /
           relative_path;
}

std::filesystem::path DefaultScreenshotPath(const Database& database) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) %
        1000;
    std::ostringstream filename;
    filename << "tradebox-" << std::put_time(&local, "%Y%m%d-%H%M%S") << '-'
             << std::setw(3) << std::setfill('0') << milliseconds.count()
             << ".bmp";
    return database.DataDirectory() / "screenshots" / filename.str();
}

bool CreateDx11RenderTarget(Dx11Renderer& renderer, std::string& error) {
    ComPtr<ID3D11Texture2D> back_buffer;
    const HRESULT result = renderer.swap_chain->GetBuffer(
        0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
    if (FAILED(result)) {
        error = "DirectX back buffer unavailable (" +
                std::to_string(static_cast<unsigned long>(result)) + ')';
        return false;
    }
    const HRESULT view_result = renderer.device->CreateRenderTargetView(
        back_buffer.Get(), nullptr, renderer.render_target.GetAddressOf());
    if (FAILED(view_result)) {
        error = "DirectX render target creation failed (" +
                std::to_string(static_cast<unsigned long>(view_result)) + ')';
        return false;
    }
    return true;
}

bool ResizeDx11Renderer(Dx11Renderer& renderer, int width, int height,
                        std::string& error) {
    if (width <= 0 || height <= 0) return true;
    if (renderer.render_target && renderer.width == width &&
        renderer.height == height)
        return true;
    renderer.context->OMSetRenderTargets(0, nullptr, nullptr);
    renderer.render_target.Reset();
    const HRESULT result = renderer.swap_chain->ResizeBuffers(
        0, static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result)) {
        error = "DirectX swap-chain resize failed (" +
                std::to_string(static_cast<unsigned long>(result)) + ')';
        return false;
    }
    renderer.width = width;
    renderer.height = height;
    return CreateDx11RenderTarget(renderer, error);
}

bool CreateDx11Renderer(SDL_Window* window, Dx11Renderer& renderer,
                        std::string& error) {
    const HWND hwnd = reinterpret_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr));
    if (hwnd == nullptr) {
        error = "SDL did not provide a Win32 window handle";
        return false;
    }
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, renderer.device.GetAddressOf(), &feature_level,
        renderer.context.GetAddressOf());
    if (FAILED(result)) {
        error = "DirectX 11 device creation failed (" +
                std::to_string(static_cast<unsigned long>(result)) + ')';
        return false;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory> factory;
    if (FAILED(renderer.device.As(&dxgi_device)) ||
        FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf())) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) {
        error = "DirectX 11 DXGI factory lookup failed";
        return false;
    }
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.OutputWindow = hwnd;
    description.Windowed = TRUE;
    // Flip presentation is the modern DWM path on supported Windows builds.
    // It reduces compositor overhead compared with the legacy blit-model
    // discard chain while retaining VSync and our software FPS fallback.
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    result = factory->CreateSwapChain(renderer.device.Get(), &description,
                                      renderer.swap_chain.GetAddressOf());
    if (FAILED(result)) {
        error = "DirectX 11 swap-chain creation failed (" +
                std::to_string(static_cast<unsigned long>(result)) + ')';
        return false;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    renderer.width = width;
    renderer.height = height;
    return CreateDx11RenderTarget(renderer, error);
}

bool CaptureDx11Framebuffer(Dx11Renderer& renderer, int width, int height,
                              const std::filesystem::path& path,
                              std::string& error) {
    if (width <= 0 || height <= 0) {
        error = "Framebuffer has no drawable area";
        return false;
    }
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            error = "Could not create screenshot directory: " +
                    filesystem_error.message();
            return false;
        }
    }

    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(renderer.swap_chain->GetBuffer(
            0, IID_PPV_ARGS(back_buffer.GetAddressOf())))) {
        error = "DirectX back buffer read failed";
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    back_buffer->GetDesc(&description);
    description.BindFlags = 0;
    description.MiscFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(renderer.device->CreateTexture2D(
            &description, nullptr, staging.GetAddressOf()))) {
        error = "DirectX screenshot staging texture creation failed";
        return false;
    }
    renderer.context->CopyResource(staging.Get(), back_buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(renderer.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0,
                                     &mapped))) {
        error = "DirectX screenshot readback failed";
        return false;
    }
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(width) * height * 4);
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    for (int row = 0; row < height; ++row) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(row) * row_bytes,
                    static_cast<const std::uint8_t*>(mapped.pData) +
                        static_cast<std::size_t>(row) * mapped.RowPitch,
                    row_bytes);
    }
    renderer.context->Unmap(staging.Get(), 0);

    SDL_Surface* surface =
        SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32,
                              pixels.data(), static_cast<int>(row_bytes));
    if (!surface) {
        error = SDL_GetError();
        return false;
    }
    const bool save_result = SDL_SaveBMP(surface, path.string().c_str());
    if (!save_result) error = SDL_GetError();
    SDL_DestroySurface(surface);
    return save_result;
}

void RefreshCoreProjection(App& app) {
    auto snapshot =
        app.application.SnapshotAfter(app.core_view.revision);
    if (!snapshot) return;

    const bool first_account =
        !app.has_account && snapshot->account.has_value();
    app.core_view = std::move(*snapshot);
    app.has_account = app.core_view.account.has_value();
    app.account = app.core_view.account.value_or(
        tradebox::core::AccountState{});
    app.positions = app.core_view.positions;
    app.orders = app.core_view.orders;
    app.positions_received_at_ms =
        app.core_view.positions_received_at_ms;
    app.orders_received_at_ms = app.core_view.orders_received_at_ms;
    app.last_account_stream_event_at_ms =
        app.core_view.last_trade_update_at_ms;

    if (first_account) {
        if (app.connection_state == ConnectionState::Connecting)
            app.connection_state =
                ConnectionState::AccountAuthenticated;
        app.database.SaveLastConnectedPaper(app.active_paper);
        const std::string account_key =
            app.account.id.empty() ? app.account.account_number
                                   : app.account.id;
        app.account_alias = app.database.LoadAccountAlias(account_key);
        if (app.account_alias.empty())
            app.account_alias =
                app.active_paper ? "Paper account" : "Live account";
        std::snprintf(app.account_alias_edit.data(),
                      app.account_alias_edit.size(), "%s",
                      app.account_alias.c_str());
        AddMessage(app, "Account authenticated: " +
                            app.account.account_number);
    }
}

void ApplyChartSnapshot(
    Chart& chart, const tradebox::core::BarSeriesSnapshot& snapshot) {
    chart.core_snapshot = snapshot;
    chart.view_series = tradebox::ui::CopyChartViewSeries(
        chart.core_snapshot);
    chart.bars = chart.view_series.bars;
    if (chart.core_snapshot.bars.empty() &&
        !chart.core_snapshot.current_bar) {
        chart.view_state = tradebox::ui::ChartViewDataState::NoData;
        chart.data_message = "No chart data available. See Activity > Events.";
    } else if (!chart.core_snapshot.missing_ranges.empty()) {
        chart.view_state = tradebox::ui::ChartViewDataState::Loading;
    } else if (chart.core_snapshot.current_bar ||
               chart.core_snapshot.latest_price) {
        chart.view_state = tradebox::ui::ChartViewDataState::Live;
        chart.data_message.clear();
    } else {
        chart.view_state = tradebox::ui::ChartViewDataState::Cached;
        chart.data_message.clear();
    }
    chart.history_loaded = chart.core_snapshot.missing_ranges.empty();
}

void RefreshRequiredMarketSymbols(App& app) {
    std::vector<std::string> required = app.watchlist;
    if (!app.selected_symbol.empty())
        required.push_back(app.selected_symbol);
    for (const auto& ticket : app.order_tickets)
        if (!ticket.draft.symbol.empty())
            required.push_back(ticket.draft.symbol);
    for (const auto& position : app.positions)
        if (!position.symbol.empty())
            required.push_back(position.symbol);
    for (const auto& order : app.orders)
        if (!order.symbol.empty())
            required.push_back(order.symbol);
    std::ranges::sort(required);
    required.erase(std::unique(required.begin(), required.end()),
                   required.end());
    if (required == app.requested_market_symbols) return;
    app.application.RefreshMarketSymbols(required);
    app.requested_market_symbols = std::move(required);
}

void RefreshUiSnapshot(App& app) {
    RefreshRequiredMarketSymbols(app);

    tradebox::application::UiSnapshotQuery query;
    query.market_symbols = app.requested_market_symbols;
    for (const auto& [symbol, chart] : app.charts) {
        if (!chart.window_open ||
            chart.requested_range.end_ns <= chart.requested_range.start_ns)
            continue;
        query.charts.push_back({
            .symbol = symbol,
            .timeframe = chart.timeframe,
            .range = chart.requested_range,
        });
    }

    app.ui_snapshot = app.application.SnapshotForUi(query);
    app.market_views.clear();
    for (const auto& market : app.ui_snapshot.markets) {
        app.market_views.insert_or_assign(market.symbol, market);
        if (market.latest_price &&
            market.latest_price->event_time_ns > 0 &&
            market.latest_price->received_at_ms > 0) {
            app.market_latency_ms =
                market.latest_price->received_at_ms -
                market.latest_price->event_time_ns / 1'000'000;
            app.last_market_received_at_ms =
                market.latest_price->received_at_ms;
        }
    }

    for (std::size_t index = 0; index < query.charts.size(); ++index) {
        const auto found = app.charts.find(query.charts[index].symbol);
        if (found != app.charts.end() && index < app.ui_snapshot.charts.size())
            ApplyChartSnapshot(found->second, app.ui_snapshot.charts[index]);
    }
}

const tradebox::core::MarketDataSnapshot& MarketView(
    App& app, const std::string& symbol) {
    auto found = app.market_views.find(symbol);
    if (found == app.market_views.end()) {
        static const tradebox::core::MarketDataSnapshot empty;
        return empty;
    }
    return found->second;
}

void DrainEvents(App& app) {
    RefreshCoreProjection(app);
    for (UiEvent& event : app.events.Drain()) {
        if (event.type == UiEventType::Status) {
            if (!event.symbol.empty() && event.message.starts_with("History")) {
                if (auto chart = app.charts.find(event.symbol);
                    chart != app.charts.end()) {
                    chart->second.view_state =
                        tradebox::ui::ChartViewDataState::NoData;
                    chart->second.data_message =
                        "Chart data request failed. See Activity > Events.";
                    chart->second.history_loaded = false;
                }
            }
            app.database.QueueTimelineEvent(
                "tradebox.connection", "", "connection_status", "",
                SystemNowMs(),
                nlohmann::json({
                    {"message", event.message},
                    {"component", OperationalComponentLabel(
                                      event.operational_component)},
                    {"state", OperationalStateLabel(
                                  event.operational_state)},
                    {"reason", OperationalReasonLabel(
                                   event.operational_reason)},
                    {"severity", OperationalSeverityLabel(
                                     event.operational_severity)},
                    {"retry_attempt", event.retry_attempt},
                    {"retry_in_ms", event.retry_in_ms},
                }).dump());
            switch (event.operational_component) {
            case OperationalComponent::Account:
                if (event.operational_state == OperationalState::Failed)
                    app.connection_state = ConnectionState::Error;
                break;
            case OperationalComponent::AccountStream:
                if (event.operational_state == OperationalState::Subscribed)
                    app.account_stream_failed = false;
                else if (
                    event.operational_state == OperationalState::Disconnected ||
                    event.operational_state == OperationalState::Failed ||
                    event.operational_state == OperationalState::Degraded)
                    app.account_stream_failed = true;
                break;
            case OperationalComponent::MarketDataStream:
                if (event.operational_state == OperationalState::Connecting)
                    app.connection_state = ConnectionState::Connecting;
                else if (event.operational_state ==
                    OperationalState::Authenticated)
                    app.connection_state = ConnectionState::Streaming;
                else if (
                    event.operational_state == OperationalState::Reconnecting)
                    app.connection_state = ConnectionState::Connecting;
                else if (
                    event.operational_state == OperationalState::Subscribed)
                    app.market_subscription_active = true;
                else if (
                    event.operational_state == OperationalState::Disconnected) {
                    app.connection_state = ConnectionState::Disconnected;
                    app.market_subscription_active = false;
                    for (auto& [symbol, chart] : app.charts) {
                        static_cast<void>(symbol);
                        chart.view_state =
                            tradebox::ui::ChartViewDataState::Stale;
                    }
                } else if (
                    event.operational_state == OperationalState::Failed ||
                    event.operational_state == OperationalState::Degraded) {
                    app.connection_state = ConnectionState::Error;
                    app.market_subscription_active = false;
                    for (auto& [symbol, chart] : app.charts) {
                        static_cast<void>(symbol);
                        chart.view_state =
                            tradebox::ui::ChartViewDataState::Stale;
                    }
                }
                break;
            case OperationalComponent::None:
                break;
            }
            AddMessage(app, std::move(event.message),
                       EventSeverityFromOperational(
                           event.operational_severity));
            continue;
        }
        if (event.type == UiEventType::MarketClock) {
            const bool first_sync = !app.has_market_clock;
            app.market_clock = event.market_clock;
            app.has_market_clock = true;
            if (first_sync) AddMessage(app, "New York market clock synchronized");
            continue;
        }
        if (event.type == UiEventType::AccountStreamConnected) {
            app.account_stream_latency_ms = event.latency_ms;
            app.account_stream_failed = false;
            app.database.QueueTimelineEvent(
                "tradebox.connection", "", "connection_status", "",
                SystemNowMs(),
                nlohmann::json({
                    {"message",
                     "Account trade_updates subscription active"},
                    {"component", OperationalComponentLabel(
                                      event.operational_component)},
                    {"state", OperationalStateLabel(
                                  event.operational_state)},
                    {"reason", OperationalReasonLabel(
                                   event.operational_reason)},
                    {"severity", OperationalSeverityLabel(
                                     event.operational_severity)},
                }).dump());
            AddMessage(app, "Account trade_updates subscription active");
            continue;
        }
        if (event.type == UiEventType::AccountStreamEvent) {
            app.last_account_stream_event_at_ms = event.received_at_ms;
            app.account_event_latency_ms = event.latency_ms;
            continue;
        }
        if (event.type == UiEventType::OrderCommandCompleted) {
            app.command_in_flight = false;
            const auto& result = event.command_result;
            if (result.outcome == tradebox::core::OrderCommandOutcome::Indeterminate)
                app.command_status = "indeterminate: " + result.message;
            else
                app.command_status = result.message.empty()
                    ? "order command completed"
                    : result.message;
            if (const auto source = app.command_sources.find(event.request_id);
                source != app.command_sources.end()) {
                app.command_status_by_source[source->second] =
                    app.command_status;
                app.command_sources.erase(source);
            }
            AddMessage(app, "Order " + event.request_id + ": " + app.command_status);
            continue;
        }
        if (event.type == UiEventType::AssetCatalogReady) {
            app.asset_catalog = std::move(event.assets);
            app.database.SaveAssetCatalog(app.asset_catalog);
            // Asset identity is now available, so a newly added watchlist
            // symbol can resolve its one-minute history request.
            for (auto& [symbol, chart] : app.charts) {
                static_cast<void>(symbol);
                if (!chart.history_loaded) RequestChartData(app, chart);
            }
            AddMessage(app, "Tradable asset catalog ready: " +
                                std::to_string(app.asset_catalog.size()) + " assets");
            continue;
        }
        // HistoricalBars and DailyBar are compatibility notifications from
        // the broker adapter. Chart data itself is read from the canonical
        // application BarSeriesSnapshot below, never projected here.
    }
    RefreshCoreProjection(app);
}

std::string NextRequestId(App& app) {
    return "gui-" + std::to_string(SystemNowMs()) + "-" +
           std::to_string(app.next_request_id++);
}

void SubmitUiCommand(App& app, tradebox::core::NativeOrderCommand command,
                     std::string request_id) {
    if (app.command_in_flight) return;
    const std::string source = std::visit(
        [](const auto& request) { return request.context.source; }, command);
    app.command_in_flight = true;
    app.command_request_id = request_id;
    app.command_sources.insert_or_assign(request_id, source);
    app.command_status_by_source.insert_or_assign(source, "Submitting...");
    auto future = app.application.SubmitOrder(std::move(command));
    app.command_workers.emplace_back([&app, request_id = std::move(request_id),
                                       future = std::move(future)]() mutable {
        UiEvent event;
        event.type = UiEventType::OrderCommandCompleted;
        event.request_id = request_id;
        event.command_result = future.get();
        app.events.Push(std::move(event));
    });
}

std::optional<std::string> DrawSymbolSelector(
    App& app, const char* label, std::array<char, 24>& input,
    std::string& previous_query,
    std::vector<tradebox::core::TradableAsset>& matches,
    int& cursor) {
    const bool submitted = ImGui::InputText(
        label, input.data(), input.size(),
        ImGuiInputTextFlags_CharsUppercase |
            ImGuiInputTextFlags_EnterReturnsTrue);
    const bool input_deactivated_after_edit =
        ImGui::IsItemDeactivatedAfterEdit();
    const bool input_active = ImGui::IsItemActive();
    const std::string query = NormalizeSymbol(input.data());
    if (query != previous_query) {
        previous_query = query;
        cursor = 0;
        matches =
            query.empty()
                ? std::vector<tradebox::core::TradableAsset>{}
                : tradebox::core::SearchTradableAssets(
                      app.asset_catalog, query, 5);
    }
    if (input_active && !matches.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            cursor = (cursor + 1) %
                     static_cast<int>(matches.size());
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            cursor =
                (cursor + static_cast<int>(matches.size()) - 1) %
                static_cast<int>(matches.size());
    }
    std::optional<std::string> clicked_match;
    if (!matches.empty()) {
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginListBox("Matches###symbol-matches",
                                ImVec2(300.0f,
                                       std::min(110.0f,
                                                22.0f * static_cast<float>(
                                                            matches.size()))))) {
        for (std::size_t index = 0; index < matches.size(); ++index) {
            const auto& asset = matches[index];
            const std::string row = asset.symbol + "  " + asset.name +
                                    "###symbol-match-" +
                                    std::to_string(index);
            if (ImGui::Selectable(row.c_str(),
                                  static_cast<int>(index) == cursor))
                clicked_match = asset.symbol;
        }
            ImGui::EndListBox();
        }
    }
    if (clicked_match) {
        std::snprintf(input.data(), input.size(), "%s",
                      clicked_match->c_str());
        previous_query = *clicked_match;
        matches.clear();
        return clicked_match;
    }
    if (!submitted && !input_deactivated_after_edit)
        return std::nullopt;
    const std::string selected =
        !matches.empty()
            ? matches[static_cast<std::size_t>(cursor)].symbol
            : query;
    if (selected.empty()) return std::nullopt;
    std::snprintf(input.data(), input.size(), "%s", selected.c_str());
    previous_query = selected;
    matches.clear();
    return selected;
}

void SelectTimeSalesSymbol(App& app, const std::string& symbol,
                           bool update_linked_ticket) {
    if (symbol.empty()) return;
    app.selected_symbol = symbol;
    if (auto chart = app.charts.find(symbol); chart != app.charts.end() &&
        !chart->second.history_loaded) {
        chart->second.view_state = tradebox::ui::ChartViewDataState::Loading;
        chart->second.data_message.clear();
        RequestChartData(app, chart->second);
    }
    std::snprintf(
        app.time_sales_symbol.data(), app.time_sales_symbol.size(),
        "%s", symbol.c_str());
    app.time_sales_query = symbol;
    app.time_sales_matches.clear();
    // A symbol chosen from any ticket must be put on the data stream now,
    // rather than waiting for the next frame's general refresh.  This matters
    // for symbols typed directly (for example, CAT), which are not necessarily
    // in the locally cached asset autocomplete list.
    RefreshRequiredMarketSymbols(app);
    if (!update_linked_ticket ||
        app.linked_order_ticket >= app.order_tickets.size())
        return;
    auto& ticket = app.order_tickets[app.linked_order_ticket];
    ticket.draft.symbol = symbol;
    std::snprintf(
        ticket.symbol.data(), ticket.symbol.size(), "%s",
        symbol.c_str());
    ticket.symbol_query = symbol;
    ticket.symbol_matches.clear();
}

void DrawOrderEntry(App& app) {
    if (!app.show_order_entry) return;
    if (app.order_tickets.empty()) app.order_tickets.emplace_back();
    bool any_ticket_open = false;
    for (std::size_t ticket_index = 0; ticket_index < app.order_tickets.size(); ++ticket_index) {
        auto& ticket = app.order_tickets[ticket_index];
        if (!ticket.window_open) continue;
        ticket.draft.name = ticket.name.data();
        ImGui::PushID(static_cast<int>(ticket_index));
        const std::string title = ticket.draft.name + "###order-ticket-" + std::to_string(ticket_index);
        if (!ticket.was_visible) {
            ImGui::SetNextWindowPos(ImVec2(1800.0f + static_cast<float>(ticket_index) * 28.0f,
                                           40.0f + static_cast<float>(ticket_index) * 28.0f),
                                    ImGuiCond_Always);
            ImGui::SetNextWindowFocus();
        }
        app.workspace.ConstrainNextWindowSize();
        if (!app.workspace.BeginWindow(
                "order-ticket-" + std::to_string(ticket_index), title,
                &ticket.window_open,
                ImVec2(1800.0f + static_cast<float>(ticket_index) * 28.0f,
                       40.0f + static_cast<float>(ticket_index) * 28.0f),
                ImVec2(360.0f, 460.0f))) {
            app.workspace.EndWindow("order-ticket-" +
                                    std::to_string(ticket_index));
            any_ticket_open = any_ticket_open || ticket.window_open;
            ImGui::PopID();
            continue;
        }
        ticket.was_visible = true;
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            app.linked_order_ticket = ticket_index;
        if (ImGui::InputText("Ticket name", ticket.name.data(), ticket.name.size()))
            ticket.draft.name = ticket.name.data();
    ticket.draft.symbol = app.selected_symbol;
    std::snprintf(ticket.symbol.data(), ticket.symbol.size(), "%s",
                  app.selected_symbol.c_str());
    ImGui::Text("Symbol  %s", app.selected_symbol.empty()
                                    ? "--"
                                    : app.selected_symbol.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("Select from Watchlist");
    const char* sides[] = {"Buy", "Sell"};
    int side_index = ticket.draft.side == "Sell" ? 1 : 0;
    if (ImGui::Combo("Side", &side_index, sides, 2)) ticket.draft.side = sides[side_index];
    ImGui::InputText("Quantity / notional", ticket.amount.data(), ticket.amount.size());
    ImGui::Checkbox("Notional", &ticket.draft.amount_is_notional);
    const char* types[] = {"Market", "Limit", "Stop", "Stop-limit"};
    int type_index = ticket.draft.type == "Limit" ? 1 : ticket.draft.type == "Stop" ? 2 : ticket.draft.type == "Stop-limit" ? 3 : 0;
    if (ImGui::Combo("Type", &type_index, types, 4)) ticket.draft.type = types[type_index];
    if (ticket.draft.type == "Limit" || ticket.draft.type == "Stop-limit")
        ImGui::InputText("Limit price", ticket.limit.data(), ticket.limit.size());
    if (ticket.draft.type == "Stop" || ticket.draft.type == "Stop-limit")
        ImGui::InputText("Stop price", ticket.stop.data(), ticket.stop.size());
    const char* tifs[] = {"Day", "Gtc"};
    int tif_index = ticket.draft.time_in_force == "Gtc" ? 1 : 0;
    if (ImGui::Combo("Time in force", &tif_index, tifs, 2)) ticket.draft.time_in_force = tifs[tif_index];
    ImGui::Checkbox("Extended hours", &ticket.draft.extended_hours);
    const auto& market = MarketView(app, ticket.draft.symbol);
    if (market.latest_quote) {
        ImGui::Text("Bid %s   Ask %s", market.latest_quote->bid_price.ToString().c_str(),
                    market.latest_quote->ask_price.ToString().c_str());
    } else ImGui::TextDisabled("Bid/ask unavailable");
    ImGui::BeginDisabled(app.command_in_flight);
    if (ImGui::Button("Send Order", ImVec2(140, 0))) {
        ticket.draft.amount = ticket.amount.data();
        ticket.draft.limit_price = ticket.limit.data();
        ticket.draft.stop_price = ticket.stop.data();
        std::vector<UiValidationMessage> errors;
        auto order = BuildNativeOrderRequest(ticket.draft, errors);
        if (!errors.empty()) {
            app.command_status = errors.front().field + ": " +
                                 errors.front().message;
        }
        else {
            const auto snapshot = app.application.Snapshot();
            const std::string request_id = NextRequestId(app);
            SubmitUiCommand(app, tradebox::core::PlaceOrderCommand{
                .context = {.request_id = request_id, .source = "gui",
                            .account_id = snapshot.account ? snapshot.account->id : "",
                            .environment = snapshot.environment,
                            .generation = snapshot.generation,
                            .live_trading_confirmed = false},
                .order = std::move(*order)}, request_id);
            app.command_status = "pending";
        }
    }
    ImGui::EndDisabled();
    if (!app.command_status.empty()) ImGui::TextWrapped("%s", app.command_status.c_str());
    app.workspace.EndWindow("order-ticket-" +
                            std::to_string(ticket_index));
        any_ticket_open = any_ticket_open || ticket.window_open;
        ImGui::PopID();
    }
    app.show_order_entry = any_ticket_open;
}

void DrawQuickOrder(App& app) {
    if (!app.show_quick_order) return;
    app.workspace.ConstrainNextWindowSize();
    if (!app.workspace.BeginWindow("tool.quick_order", "QUICK ORDER",
                                   &app.show_quick_order,
                                   ImVec2(1500.0f, 10.0f),
                                   ImVec2(360.0f, 350.0f))) {
        app.workspace.EndWindow("tool.quick_order");
        return;
    }

    const std::string& symbol = app.selected_symbol;
    ImGui::Text("Symbol  %s", symbol.empty() ? "--" : symbol.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("Select from Watchlist");
    const auto& market = MarketView(app, symbol);
    ImGui::SetNextItemWidth(130.0f);
    ImGui::InputFloat("Long BP %", &app.quick_long_buying_power_percent,
                      1.0f, 5.0f, "%.0f%%");
    app.quick_long_buying_power_percent = std::clamp(
        app.quick_long_buying_power_percent, 1.0f, 100.0f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::InputFloat("Short BP %", &app.quick_short_buying_power_percent,
                      1.0f, 5.0f, "%.0f%%");
    app.quick_short_buying_power_percent = std::clamp(
        app.quick_short_buying_power_percent, 1.0f, 100.0f);
    const auto long_percent = tradebox::core::Decimal::Parse(
        std::to_string(app.quick_long_buying_power_percent));
    const auto short_percent = tradebox::core::Decimal::Parse(
        std::to_string(app.quick_short_buying_power_percent));
    const auto quick = app.has_account && long_percent && short_percent
        ? tradebox::core::ProjectQuickOrder(
              app.account, app.positions, market, *long_percent,
              *short_percent)
        : tradebox::core::QuickOrderProjection{};
    const double buy_price = quick.reference_price.ToDisplayDouble();
    const double sell_price = buy_price;
    const double buying_power = app.has_account
        ? app.account.buying_power.ToDisplayDouble() : 0.0;
    const double long_budget = quick.long_budget.ToDisplayDouble();
    const double short_budget = quick.short_budget.ToDisplayDouble();
    const bool has_long = quick.long_exit_quantity.has_value();
    const bool has_short = quick.short_exit_quantity.has_value();

    ImGui::TextDisabled(app.active_paper ? "PAPER MARKET ORDER"
                                         : "LIVE QUICK ORDERS DISABLED");
    ImGui::Text("Buying power  $%.2f", buying_power);
    ImGui::TextDisabled("Long budget $%.2f    Short budget $%.2f",
                        long_budget, short_budget);
    ImGui::Text("Last trade  $%.2f", buy_price);
    if (symbol.empty())
        ImGui::TextDisabled("Enter a ticker symbol.");
    else if (market.stream_status == tradebox::core::MarketStreamStatus::Error)
        ImGui::TextColored(ImVec4(1.0f, .45f, .35f, 1.0f),
                           "%s market-data error: %s", symbol.c_str(),
                           market.status_message.empty()
                               ? "see Time & Sales"
                               : market.status_message.c_str());
    else if (!market.trades_subscribed)
        ImGui::TextDisabled("Subscribing to %s trades...", symbol.c_str());
    else if (buy_price <= 0.0 || sell_price <= 0.0)
        ImGui::TextDisabled("Subscribed to %s; waiting for a last trade.",
                            symbol.c_str());
    ImGui::Separator();

    const auto submit_market = [&app, &symbol](
                                   tradebox::core::OrderSide side,
                                   const tradebox::core::Decimal& quantity,
                                   const char* action) {
        const auto snapshot = app.application.Snapshot();
        const std::string request_id = NextRequestId(app);
        tradebox::core::NativeOrderRequest order;
        order.asset_class = tradebox::core::AssetClass::Equity;
        order.symbol = symbol;
        order.qty = quantity;
        order.side = side;
        order.type = tradebox::core::OrderType::Market;
        order.time_in_force = tradebox::core::TimeInForce::Day;
        order.order_class = tradebox::core::OrderClass::Simple;
        SubmitUiCommand(app, tradebox::core::PlaceOrderCommand{
            .context = {.request_id = request_id, .source = "quick-order",
                        .account_id = snapshot.account ? snapshot.account->id : "",
                        .environment = snapshot.environment,
                        .generation = snapshot.generation,
                        .live_trading_confirmed = false},
            .order = std::move(order)}, request_id);
        app.command_status = std::string("pending ") + action;
    };
    const bool paper_ready = app.active_paper && app.has_account &&
                             !app.command_in_flight;
    ImGui::BeginDisabled(!paper_ready ||
                         quick.long_quantity <= tradebox::core::Decimal::Zero());
    if (ImGui::Button("LONG MAX BP", ImVec2(165, 0))) {
        submit_market(tradebox::core::OrderSide::Buy,
                      quick.long_quantity, "long max BP");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!paper_ready ||
                         quick.short_quantity <= tradebox::core::Decimal::Zero());
    if (ImGui::Button("SHORT MAX BP", ImVec2(165, 0))) {
        submit_market(tradebox::core::OrderSide::Sell,
                      quick.short_quantity, "short max BP");
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!paper_ready || !has_long);
    if (ImGui::Button("EXIT LONG FULL", ImVec2(165, 0)))
        submit_market(tradebox::core::OrderSide::Sell,
                      *quick.long_exit_quantity, "exit long");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!paper_ready || !has_short);
    if (ImGui::Button("COVER SHORT FULL", ImVec2(165, 0)))
        submit_market(tradebox::core::OrderSide::Buy,
                      *quick.short_exit_quantity, "cover short");
    ImGui::EndDisabled();
    if (!app.active_paper)
        ImGui::TextDisabled("Use the standard ticket for live orders.");
    else if (!app.has_account)
        ImGui::TextDisabled("Connect the paper account to enable quick orders.");
    else if (!app.command_status.empty())
        ImGui::TextDisabled("%s", app.command_status.c_str());
    app.workspace.EndWindow("tool.quick_order");
}

void DrawOcoOrder(App& app) {
    if (!app.show_oco_order) return;
    app.workspace.ConstrainNextWindowSize();
    if (!app.workspace.BeginWindow("tool.oco_order", "BRACKET ORDER",
                                   &app.show_oco_order,
                                   ImVec2(1500.0f, 370.0f),
                                   ImVec2(430.0f, 390.0f))) {
        app.workspace.EndWindow("tool.oco_order");
        return;
    }

    // A bracket ticket always follows the workstation's one active symbol.
    // It must not compete with Quick Order or Time & Sales for ticker state.
    const std::string& symbol = app.selected_symbol;
    App::OcoDraft& draft = app.oco_drafts[symbol];
    const auto& market = MarketView(app, symbol);
    const auto long_percent = tradebox::core::Decimal::Parse(
        std::to_string(app.quick_long_buying_power_percent));
    const auto short_percent = tradebox::core::Decimal::Parse(
        std::to_string(app.quick_short_buying_power_percent));
    const auto quick = app.has_account && long_percent && short_percent
        ? tradebox::core::ProjectQuickOrder(
              app.account, app.positions, market, *long_percent,
              *short_percent)
        : tradebox::core::QuickOrderProjection{};
    const double reference = quick.reference_price.ToDisplayDouble();
    const float buying_power_percent = draft.short_entry
        ? app.quick_short_buying_power_percent
        : app.quick_long_buying_power_percent;
    const double budget = draft.short_entry
        ? quick.short_budget.ToDisplayDouble()
        : quick.long_budget.ToDisplayDouble();
    const tradebox::core::Decimal& quantity = draft.short_entry
        ? quick.short_quantity : quick.long_quantity;
    const auto target_percent = tradebox::core::Decimal::Parse(
        std::to_string(draft.target_percent));
    const auto stop_percent = tradebox::core::Decimal::Parse(
        std::to_string(draft.stop_percent));
    const auto bracket = target_percent && stop_percent
        ? tradebox::core::ProjectBracket(
              quick.reference_price, *target_percent, *stop_percent,
              quantity, draft.short_entry)
        : tradebox::core::BracketProjection{};

    ImGui::TextDisabled("PAPER BRACKET: market entry + linked OCO exits");
    ImGui::Text("%s", symbol.empty() ? "No selected symbol" : symbol.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("Change symbol in Quick Order or Time & Sales");
    if (ImGui::RadioButton("Long", !draft.short_entry))
        draft.short_entry = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Short", draft.short_entry))
        draft.short_entry = true;
    ImGui::Text("Entry reference  $%.4f    Budget  $%.2f", reference, budget);
    ImGui::Text("Maximum quantity  %s  (%0.0f%% buying power)",
                quantity.ToString().c_str(), buying_power_percent);

    ImGui::SeparatorText("Attached exits");
    ImGui::SetNextItemWidth(230);
    ImGui::SliderFloat("Target %", &draft.target_percent, 0.05f, 5.0f,
                       "%.2f%%");
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f)
        draft.target_percent = std::clamp(
            draft.target_percent + ImGui::GetIO().MouseWheel * 0.05f,
            0.05f, 5.0f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputFloat("##target-direct", &draft.target_percent, 0.05f, 0.25f,
                      "%.2f%%");
    draft.target_percent = std::clamp(draft.target_percent, 0.05f, 5.0f);
    ImGui::SetNextItemWidth(230);
    ImGui::SliderFloat("Stop %", &draft.stop_percent, 0.05f, 5.0f, "%.2f%%");
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f)
        draft.stop_percent = std::clamp(
            draft.stop_percent + ImGui::GetIO().MouseWheel * 0.05f,
            0.05f, 5.0f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputFloat("##stop-direct", &draft.stop_percent, 0.05f, 0.25f,
                      "%.2f%%");
    draft.stop_percent = std::clamp(draft.stop_percent, 0.05f, 5.0f);
    ImGui::Checkbox("GTC", &draft.gtc);

    ImGui::Text("Target  $%.4f     Stop  $%.4f",
                bracket.target_price.ToDisplayDouble(),
                bracket.stop_price.ToDisplayDouble());
    ImGui::Text("Reward  $%.2f     Risk  $%.2f     R:R  %.2f",
                bracket.reward.ToDisplayDouble(), bracket.risk.ToDisplayDouble(),
                bracket.risk_reward.ToDisplayDouble());

    std::string preflight_error;
    std::optional<tradebox::core::NativeOrderRequest> prepared_order;
    if (!app.active_paper)
        preflight_error = "Bracket entry is enabled only for the paper account.";
    else if (!app.has_account)
        preflight_error = "Connect the paper account before placing a bracket.";
    else if (symbol.empty() || quantity <= tradebox::core::Decimal::Zero() ||
             reference <= 0.0 || !target_percent || !stop_percent)
        preflight_error = "Waiting for a selected symbol, account, and market price.";
    else {
        tradebox::core::NativeOrderRequest order;
        order.asset_class = tradebox::core::AssetClass::Equity;
        order.symbol = symbol;
        order.qty = quantity;
        order.side = draft.short_entry ? tradebox::core::OrderSide::Sell
                                       : tradebox::core::OrderSide::Buy;
        order.type = tradebox::core::OrderType::Market;
        order.time_in_force = draft.gtc ? tradebox::core::TimeInForce::Gtc
                                         : tradebox::core::TimeInForce::Day;
        order.order_class = tradebox::core::OrderClass::Bracket;
        order.take_profit = tradebox::core::TakeProfit{bracket.target_price};
        order.stop_loss =
            tradebox::core::StopLoss{bracket.stop_price, std::nullopt};
        prepared_order = std::move(order);
    }
    const bool paper_ready = prepared_order.has_value() &&
                             !app.command_in_flight;
    ImGui::BeginDisabled(!paper_ready);
    if (ImGui::Button(draft.short_entry ? "ENTER SHORT + EXITS"
                                        : "ENTER LONG + EXITS",
                      ImVec2(220, 0))) {
        const auto snapshot = app.application.Snapshot();
        const std::string request_id = NextRequestId(app);
        SubmitUiCommand(app, tradebox::core::PlaceOrderCommand{
            .context = {.request_id = request_id, .source = "bracket-order",
                        .account_id = snapshot.account ? snapshot.account->id : "",
                        .environment = snapshot.environment,
                        .generation = snapshot.generation,
                        .live_trading_confirmed = false},
            .order = std::move(*prepared_order)}, request_id);
    }
    ImGui::EndDisabled();
    if (!preflight_error.empty())
        ImGui::TextDisabled("%s", preflight_error.c_str());
    if (const auto status = app.command_status_by_source.find("bracket-order");
        status != app.command_status_by_source.end())
        ImGui::TextWrapped("Bracket: %s", status->second.c_str());
    app.workspace.EndWindow("tool.oco_order");
}

std::string TradeTime(const std::string& timestamp) {
    const std::size_t separator = timestamp.find('T');
    if (separator == std::string::npos ||
        separator + 9 > timestamp.size())
        return timestamp;
    const std::size_t available =
        std::min<std::size_t>(12, timestamp.size() - separator - 1);
    return timestamp.substr(separator + 1, available);
}

void DrawTimeSales(App& app) {
    if (!app.show_time_sales) return;
    app.workspace.ConstrainNextWindowSize();
    if (!app.workspace.BeginWindow("tool.time_sales", "TIME & SALES",
                                   &app.show_time_sales,
                                   ImVec2(10.0f, 750.0f),
                                   ImVec2(560.0f, 420.0f))) {
        app.workspace.EndWindow("tool.time_sales");
        return;
    }
    ImGui::SetNextItemWidth(170);
    if (const auto selected = DrawSymbolSelector(
            app, "Symbol", app.time_sales_symbol,
            app.time_sales_query, app.time_sales_matches,
            app.time_sales_cursor))
        SelectTimeSalesSymbol(app, *selected, true);

    const auto& market = MarketView(app, app.selected_symbol);
    const bool stale = market.last_received_at_ms > 0 &&
        SystemNowMs() - market.last_received_at_ms > 5000;
    const char* stream = market.stream_status == tradebox::core::MarketStreamStatus::Subscribed
        ? "CONNECTED" : market.stream_status == tradebox::core::MarketStreamStatus::Stale
        ? "STALE" : "DISCONNECTED";
    const char* feed =
        market.feed == tradebox::core::MarketDataFeed::Sip
            ? "SIP"
            : market.feed == tradebox::core::MarketDataFeed::Iex
                  ? "IEX"
                  : "UNKNOWN FEED";
    ImGui::SameLine();
    ImGui::TextColored(stale ? ImVec4(1, .65f, .2f, 1) : ImVec4(.3f, .9f, .5f, 1),
                       "%s | %s | %s", feed, stream,
                       market.trades_subscribed && market.quotes_subscribed ? "SUBSCRIBED" : "NOT SUBSCRIBED");
    if (market.latest_quote) {
        const auto& quote = *market.latest_quote;
        ImGui::Text("Bid %s x %s (%s)    Ask %s x %s (%s)",
                    quote.bid_price.ToString().c_str(), quote.bid_size.ToString().c_str(), quote.bid_exchange.c_str(),
                    quote.ask_price.ToString().c_str(), quote.ask_size.ToString().c_str(), quote.ask_exchange.c_str());
    }
    ImGui::TextDisabled(
        "%zu prints | newest first", market.trades.size());
    if (ImGui::BeginTable("trade-tape", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                         ImGuiTableFlags_Hideable |
                                         ImGuiTableFlags_Reorderable,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Price"); ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Exchange"); ImGui::TableSetupColumn("Conditions"); ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(market.trades.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart;
                 row < clipper.DisplayEnd; ++row) {
                const std::size_t index =
                    static_cast<std::size_t>(row);
                const auto& trade = market.trades[index];
                int direction = 0;
                if (index + 1 < market.trades.size()) {
                    const auto& older = market.trades[index + 1];
                    direction =
                        trade.price > older.price
                            ? 1
                            : trade.price < older.price ? -1 : 0;
                }
                const ImVec4 color =
                    trade.corrected
                        ? ImVec4(1, .7f, .25f, 1)
                        : direction > 0
                              ? ImVec4(.25f, .9f, .58f, 1)
                              : direction < 0
                                    ? ImVec4(.96f, .38f, .43f, 1)
                                    : ImVec4(.75f, .85f, 1, 1);
                const std::string time =
                    TradeTime(trade.broker_timestamp);
                const std::string price =
                    trade.price.ToString();
                const std::string size =
                    trade.size.ToString();
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(time.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(
                    color, "%s %s",
                    direction > 0 ? "^" : direction < 0 ? "v" : " ",
                    price.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(size.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(trade.exchange.c_str());
                ImGui::TableNextColumn();
                std::string conditions;
                for (const auto& condition : trade.conditions) {
                    if (!conditions.empty()) conditions += ",";
                    conditions += condition;
                }
                ImGui::TextUnformatted(conditions.c_str());
            }
        }
        ImGui::EndTable();
    }
    app.workspace.EndWindow("tool.time_sales");
}

void DrawConnectionBadge(const App& app) {
    if (app.has_account) {
        ImGui::TextColored(ImVec4(0.28f, 0.88f, 0.52f, 1.0f),
                           "ACCOUNT CONNECTED");
        ImGui::SameLine();
        if (app.connection_state == ConnectionState::Streaming &&
            app.market_subscription_active) {
            ImGui::TextDisabled("| MARKET DATA LIVE");
        } else {
            ImGui::TextDisabled("| MARKET DATA CONNECTING");
        }
        return;
    }
    const bool connected =
        app.connection_state == ConnectionState::Streaming ||
        app.connection_state == ConnectionState::AccountAuthenticated;
    const char* label = "DISCONNECTED";
    if (app.connection_state == ConnectionState::Connecting)
        label = "CONNECTING";
    else if (app.connection_state == ConnectionState::AccountAuthenticated)
        label = "ACCOUNT AUTHENTICATED";
    else if (app.connection_state == ConnectionState::Streaming)
        label = "MARKET STREAMING";
    else if (app.connection_state == ConnectionState::Error)
        label = "CONNECTION ERROR";
    ImGui::TextColored(
        connected ? ImVec4(0.28f, 0.88f, 0.52f, 1.0f)
                  : ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
        "%s", label);
}

void ConnectSavedAccount(App& app, bool paper) {
    AlpacaCredentials credentials;
    std::string error;
    if (CredentialStore::Load(paper, credentials, error)) {
        app.has_account = false;
        app.positions.clear();
        app.orders.clear();
        app.positions_received_at_ms = 0;
        app.orders_received_at_ms = 0;
        app.active_paper = paper;
        app.connection_state = ConnectionState::Connecting;
        app.market_latency_ms = -1;
        app.last_market_received_at_ms = 0;
        app.market_subscription_active = false;
        app.has_market_clock = false;
        app.market_clock = {};
        app.account_stream_latency_ms = -1;
        app.last_account_stream_event_at_ms = 0;
        app.account_event_latency_ms = -1;
        app.account_stream_failed = false;
        const auto core_result = app.application.Connect({
            .environment =
                paper ? tradebox::core::AccountEnvironment::Paper
                      : tradebox::core::AccountEnvironment::Live,
            .api_key = credentials.key,
            .api_secret = credentials.secret,
            .market_symbols = app.watchlist,
        });
        if (!core_result || !core_result->accepted) {
            AddMessage(app, !core_result
                                ? core_result.error().message
                                : core_result->message);
            return;
        }
        app.application.RefreshAssetCatalog();
        AddMessage(app, std::string("Connecting ") +
                            (paper ? "paper" : "live") + " account...");
    } else {
        AddMessage(app, error);
        app.credential_paper = paper;
        app.credentials_open = true;
    }
}

void DisconnectAccount(App& app) {
    const auto core_result = app.application.Disconnect();
    if (!core_result)
        AddMessage(app, "Core disconnect failed: " +
                            core_result.error().message);
    app.has_account = false;
    app.account = {};
    app.account_alias.clear();
    app.account_alias_edit.fill('\0');
    app.positions.clear();
    app.orders.clear();
    app.positions_received_at_ms = 0;
    app.orders_received_at_ms = 0;
    app.connection_state = ConnectionState::Disconnected;
    app.market_latency_ms = -1;
    app.last_market_received_at_ms = 0;
    app.market_subscription_active = false;
    app.requested_market_symbols.clear();
    app.has_market_clock = false;
    app.market_clock = {};
    app.account_stream_latency_ms = -1;
    app.last_account_stream_event_at_ms = 0;
    app.account_event_latency_ms = -1;
    app.account_stream_failed = false;
    AddMessage(app, "Disconnected");
}

void DrawAccount(App& app) {
    if (!app.show_account) return;
    app.workspace.ConstrainNextWindowSize(ImVec2(320.0f, 260.0f));
    if (!app.workspace.BeginWindow("tool.account", "ACCOUNT",
                                   &app.show_account,
                                   ImVec2(10.0f, 10.0f),
                                   ImVec2(420.0f, 420.0f))) {
        app.workspace.EndWindow("tool.account");
        return;
    }
    DrawConnectionBadge(app);
    if (app.has_account) {
        ImGui::Separator();
        ImGui::TextColored(
            app.active_paper ? ImVec4(0.3f, 0.85f, 0.55f, 1)
                             : ImVec4(1.0f, 0.35f, 0.32f, 1),
            app.active_paper ? "PAPER ACCOUNT" : "LIVE ACCOUNT / READ ONLY");
        ImGui::TextDisabled("Local account name");
        ImGui::SetNextItemWidth(
            std::max(120.0f, ImGui::GetContentRegionAvail().x - 100.0f));
        ImGui::InputText("##account-name", app.account_alias_edit.data(),
                         app.account_alias_edit.size());
        ImGui::SameLine();
        const std::string edited_alias = app.account_alias_edit.data();
        ImGui::BeginDisabled(edited_alias.empty() ||
                             edited_alias == app.account_alias);
        if (ImGui::Button("Save", ImVec2(88.0f, 0))) {
            const std::string account_key =
                app.account.id.empty() ? app.account.account_number
                                       : app.account.id;
            app.account_alias = edited_alias;
            app.database.SaveAccountAlias(
                account_key, app.account.account_number, app.account_alias);
            AddMessage(app, "Account name saved locally");
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                "Alpaca's Trading API does not expose the dashboard nickname. "
                "This workstation name is stored locally for this account ID.");
        ImGui::Text("Account  %s", app.account.account_number.c_str());
        ImGui::Text("Alpaca status  %s", app.account.status.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "ACTIVE is Alpaca's trading-account eligibility state, not "
                "the connection state.");
        ImGui::Text("Equity   $%s",
                    app.account.equity.ToString().c_str());
        const double equity = app.account.equity.ToDisplayDouble();
        const double last_equity =
            app.account.last_equity.ToDisplayDouble();
        const double daily_pl = equity - last_equity;
        const double daily_plpc =
            last_equity != 0
                ? daily_pl / last_equity * 100.0
                : 0;
        ImGui::TextColored(
            daily_pl >= 0 ? ImVec4(0.25f, 0.9f, 0.58f, 1)
                          : ImVec4(0.96f, 0.38f, 0.43f, 1),
            "Today    %+.2f  (%+.2f%%)", daily_pl, daily_plpc);
        ImGui::Text("Cash     $%s", app.account.cash.ToString().c_str());
        ImGui::Text("Buying power  $%s",
                    app.account.buying_power.ToString().c_str());
        ImGui::TextDisabled("Account snapshot loaded");
    } else {
        ImGui::TextDisabled("Use Menu > Account to connect.");
    }
    app.workspace.EndWindow("tool.account");
}

enum class IndicatorVisual {
    Disconnected,
    Connecting,
    Connected,
    Delayed
};

std::string FormatDelay(std::int64_t milliseconds) {
    if (milliseconds < 0) return "--";
    if (milliseconds < 1000) return std::to_string(milliseconds) + "ms";
    std::ostringstream value;
    value << std::fixed << std::setprecision(1)
          << static_cast<double>(milliseconds) / 1000.0 << 's';
    return value.str();
}

void DrawStatusIndicator(const char* id, const std::string& text,
                         IndicatorVisual visual, const std::string& tooltip) {
    ImVec4 color;
    if (visual == IndicatorVisual::Connected) {
        color = ImVec4(0.20f, 0.88f, 0.48f, 1.0f);
    } else if (visual == IndicatorVisual::Disconnected) {
        color = ImVec4(0.95f, 0.25f, 0.28f, 1.0f);
    } else {
        float alpha = 1.0f;
        if (visual == IndicatorVisual::Connecting) {
            const float phase = static_cast<float>(SDL_GetTicks() % 1200) /
                                1200.0f;
            alpha = 0.45f + 0.55f *
                                (0.5f + 0.5f *
                                            std::sin(phase * 2.0f * 3.14159265f));
        }
        color = ImVec4(1.0f, 0.72f, 0.16f, alpha);
    }

    ImGui::PushID(id);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
    ImGui::InvisibleButton("##health",
                           ImVec2(text_size.x + 10.0f,
                                  ImGui::GetFrameHeight()));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(start.x, start.y + 3.0f),
                        ImVec2(start.x + 3.0f,
                               start.y + ImGui::GetFrameHeight() - 3.0f),
                        ImGui::ColorConvertFloat4ToU32(color), 1.0f);
    draw->AddText(ImVec2(start.x + 8.0f,
                         start.y + (ImGui::GetFrameHeight() - text_size.y) * 0.5f),
                  IM_COL32(205, 211, 222, 255), text.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip.c_str());
    ImGui::PopID();
}

std::string FormatCountdown(std::int64_t milliseconds) {
    const std::int64_t total_seconds = std::max<std::int64_t>(0, milliseconds / 1000);
    const std::int64_t days = total_seconds / 86400;
    const std::int64_t hours = (total_seconds / 3600) % 24;
    const std::int64_t minutes = (total_seconds / 60) % 60;
    const std::int64_t seconds = total_seconds % 60;
    char value[48];
    if (days > 0)
        std::snprintf(value, sizeof(value), "%lldd %02lld:%02lld:%02lld",
                      days, hours, minutes, seconds);
    else
        std::snprintf(value, sizeof(value), "%02lld:%02lld:%02lld",
                      total_seconds / 3600, minutes, seconds);
    return value;
}

bool NewYorkSystemTime(std::int64_t epoch_ms, SYSTEMTIME& local) {
    static const DYNAMIC_TIME_ZONE_INFORMATION eastern = [] {
        DYNAMIC_TIME_ZONE_INFORMATION result{};
        for (DWORD index = 0;; ++index) {
            DYNAMIC_TIME_ZONE_INFORMATION candidate{};
            if (EnumDynamicTimeZoneInformation(index, &candidate) ==
                ERROR_NO_MORE_ITEMS)
                break;
            if (std::wcscmp(candidate.TimeZoneKeyName,
                            L"Eastern Standard Time") == 0) {
                result = candidate;
                break;
            }
        }
        return result;
    }();
    const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm utc_tm{};
    gmtime_s(&utc_tm, &seconds);
    SYSTEMTIME utc{};
    utc.wYear = static_cast<WORD>(utc_tm.tm_year + 1900);
    utc.wMonth = static_cast<WORD>(utc_tm.tm_mon + 1);
    utc.wDay = static_cast<WORD>(utc_tm.tm_mday);
    utc.wDayOfWeek = static_cast<WORD>(utc_tm.tm_wday);
    utc.wHour = static_cast<WORD>(utc_tm.tm_hour);
    utc.wMinute = static_cast<WORD>(utc_tm.tm_min);
    utc.wSecond = static_cast<WORD>(utc_tm.tm_sec);
    utc.wMilliseconds = static_cast<WORD>(epoch_ms % 1000);
    return SystemTimeToTzSpecificLocalTimeEx(&eastern, &utc, &local) != FALSE;
}

void DrawPerformanceOverlay(const App& app) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float fps = ImGui::GetIO().Framerate;
    const float frame_ms = fps > 0 ? 1000.0f / fps : 0;
    const std::int64_t system_now = SystemNowMs();
    std::int64_t market_now = system_now;
    if (app.has_market_clock)
        market_now = app.market_clock.timestamp_ms +
                     (system_now - app.market_clock.received_at_ms);

    SYSTEMTIME new_york{};
    char clock_label[64] = "NEW YORK  --:--:-- ET";
    if (NewYorkSystemTime(market_now, new_york))
        std::snprintf(clock_label, sizeof(clock_label),
                      "NEW YORK  %02u:%02u:%02u ET", new_york.wHour,
                      new_york.wMinute, new_york.wSecond);

    std::string session_label = "MARKET CLOCK WAITING";
    if (app.has_market_clock) {
        constexpr std::int64_t premarket_before_open =
            5LL * 60 * 60 * 1000 + 30LL * 60 * 1000;
        const std::int64_t premarket_open =
            app.market_clock.next_open_ms - premarket_before_open;
        if (app.market_clock.is_open) {
            session_label =
                "MARKET OPEN  |  CLOSE IN " +
                FormatCountdown(app.market_clock.next_close_ms - market_now);
        } else if (market_now < premarket_open) {
            session_label =
                "MARKET CLOSED  |  PREMARKET IN " +
                FormatCountdown(premarket_open - market_now) +
                "  |  OPEN IN " +
                FormatCountdown(app.market_clock.next_open_ms - market_now);
        } else if (market_now < app.market_clock.next_open_ms) {
            session_label =
                "PREMARKET  |  OPEN IN " +
                FormatCountdown(app.market_clock.next_open_ms - market_now);
        } else {
            session_label = "MARKET CLOSED  |  CLOCK REFRESHING";
        }
    }

    struct UsageSample {
        ULONGLONG process_time = 0;
        ULONGLONG system_time = 0;
        Uint64 sampled_at = 0;
        float total_cpu_percent = 0;
        float core_equivalents = 0;
        std::size_t working_set_bytes = 0;
    };
    static UsageSample usage;
    const auto file_time_value = [](const FILETIME& value) {
        ULARGE_INTEGER result{};
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
    };
    const Uint64 usage_now = SDL_GetTicks();
    if (usage.sampled_at == 0 || usage_now - usage.sampled_at >= 500) {
        FILETIME created{}, exited{}, process_kernel{}, process_user{};
        FILETIME idle{}, system_kernel{}, system_user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited,
                            &process_kernel, &process_user) &&
            GetSystemTimes(&idle, &system_kernel, &system_user)) {
            const ULONGLONG process_time =
                file_time_value(process_kernel) + file_time_value(process_user);
            const ULONGLONG system_time =
                file_time_value(system_kernel) + file_time_value(system_user);
            if (usage.process_time > 0 && system_time > usage.system_time) {
                usage.total_cpu_percent =
                    100.0f * static_cast<float>(
                                 static_cast<double>(
                                     process_time - usage.process_time) /
                                 static_cast<double>(
                                     system_time - usage.system_time));
                const DWORD logical_processors =
                    std::max<DWORD>(1, GetActiveProcessorCount(
                                           ALL_PROCESSOR_GROUPS));
                usage.core_equivalents =
                    usage.total_cpu_percent * logical_processors / 100.0f;
            }
            usage.process_time = process_time;
            usage.system_time = system_time;
        }
        PROCESS_MEMORY_COUNTERS memory{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &memory,
                                 sizeof(memory)))
            usage.working_set_bytes = memory.WorkingSetSize;
        usage.sampled_at = usage_now;
    }

    char performance_label[160];
    const char* sync_label =
        app.vsync_enabled
            ? "VSYNC ON"
            : (app.maximum_frame_rate > 0
                   ? "FPS CAPPED"
                   : (app.vsync_requested ? "VSYNC FAILED" : "UNCAPPED"));
    std::snprintf(performance_label, sizeof(performance_label),
                  "%s %.0f Hz  |  %.0f FPS  %.2f ms  |  CPU %.1f%% "
                  "(%.2f core)  |  RAM %.0f MB",
                  sync_label, app.display_refresh_hz, fps, frame_ms,
                  usage.total_cpu_percent,
                  usage.core_equivalents,
                  static_cast<double>(usage.working_set_bytes) /
                      (1024.0 * 1024.0));
    const auto storage_size = [](std::uint64_t bytes) {
        constexpr double kMegabyte = 1024.0 * 1024.0;
        constexpr double kGigabyte = 1024.0 * kMegabyte;
        char value[32];
        if (bytes >= static_cast<std::uint64_t>(kGigabyte))
            std::snprintf(value, sizeof(value), "%.2f GB",
                          static_cast<double>(bytes) / kGigabyte);
        else
            std::snprintf(value, sizeof(value), "%.1f MB",
                          static_cast<double>(bytes) / kMegabyte);
        return std::string(value);
    };
    const std::string storage_label =
        "MARKET DATA  |  CANDLES " +
        storage_size(app.market_storage.candlestick_bytes) + " (" +
        std::to_string(app.market_storage.candlestick_rows) +
        ")  |  TICKS " + storage_size(app.market_storage.tick_bytes) + " (" +
        std::to_string(app.market_storage.tick_rows) + ")  |  DISK " +
        storage_size(app.market_storage.database_bytes);
    const std::string writer_label =
        "WRITE-BEHIND  |  QUEUED " +
        std::to_string(app.writer_telemetry.pending_events) +
        "  |  PEAK " +
        std::to_string(app.writer_telemetry.high_water_events) +
        "  |  DROPPED " +
        std::to_string(
            app.writer_telemetry.dropped_market_events +
            app.writer_telemetry.dropped_timeline_events);
    const std::array<std::string_view, 5> lines = {
        clock_label, session_label, performance_label, storage_label,
        writer_label};
    float text_width = 0;
    for (const std::string_view line : lines)
        text_width =
            std::max(text_width, ImGui::CalcTextSize(line.data()).x);
    const float line_height = ImGui::GetTextLineHeight();
    const ImVec2 padding(7, 4);
    const ImVec2 bottom_right(
        viewport->WorkPos.x + viewport->WorkSize.x - 8,
        viewport->WorkPos.y + viewport->WorkSize.y - 8);
    const ImVec2 top_left(
        bottom_right.x - text_width - padding.x * 2,
        bottom_right.y - line_height * static_cast<float>(lines.size()) -
            padding.y * 2);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(top_left, bottom_right, IM_COL32(12, 16, 23, 220), 3);
    draw->AddRect(top_left, bottom_right, IM_COL32(55, 66, 82, 220), 3);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const ImU32 color =
            index == 1
                ? (app.has_market_clock && app.market_clock.is_open
                       ? IM_COL32(71, 224, 133, 255)
                       : IM_COL32(235, 191, 92, 255))
                : IM_COL32(190, 202, 220, 255);
        draw->AddText(
            ImVec2(top_left.x + padding.x,
                   top_left.y + padding.y + line_height * index),
            color, lines[index].data());
    }
}

void DrawConnectionLight(const App& app, float height) {
    const std::int64_t now = SystemNowMs();
    const std::int64_t account_age =
        app.account.received_at_ms > 0 ? now - app.account.received_at_ms : -1;
    const std::int64_t positions_age =
        app.positions_received_at_ms > 0 ? now - app.positions_received_at_ms
                                         : -1;
    const std::int64_t clock_age =
        app.market_clock.received_at_ms > 0
            ? now - app.market_clock.received_at_ms
            : -1;
    const bool restrictions =
        app.has_account &&
        (app.account.account_blocked || app.account.trade_suspended_by_user ||
         app.account.trading_blocked);
    const bool core_live =
        app.core_view.safety_status ==
        tradebox::core::SafetyStatus::Live;
    const bool healthy =
        app.market_subscription_active && core_live &&
        app.has_account && app.has_market_clock &&
        app.core_view.trading_permitted && !restrictions &&
        account_age >= 0 && account_age < 90000 &&
        positions_age >= 0 && positions_age < 90000 &&
        app.orders_received_at_ms > 0 && clock_age >= 0 && clock_age < 90000;
    const bool disconnected =
        app.connection_state == ConnectionState::Disconnected ||
        app.connection_state == ConnectionState::Error;
    const ImU32 color =
        healthy ? IM_COL32(62, 220, 126, 255)
                : (disconnected ? IM_COL32(239, 68, 79, 255)
                                : IM_COL32(245, 184, 63, 255));

    ImGui::PushID("connection-health");
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##light", ImVec2(22.0f, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center(start.x + 11.0f, start.y + height * 0.5f);
    draw->AddCircleFilled(center, 5.0f, color, 24);
    draw->AddCircle(center, 6.5f, IM_COL32(15, 19, 27, 255), 24, 1.5f);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(healthy ? "CONNECTION HEALTHY"
                                      : "CONNECTION NEEDS ATTENTION");
        ImGui::Separator();
        ImGui::Text("Market data       %s",
                    app.market_subscription_active ? "SUBSCRIBED"
                                                   : "NOT SUBSCRIBED");
        ImGui::Text("Trading stream    %s",
                    app.core_view.trade_updates_acknowledged
                        ? "ACKNOWLEDGED"
                        : "NOT ACKNOWLEDGED");
        ImGui::Text("Core safety       %s",
                    app.core_view.status_message.c_str());
        ImGui::Text("Account snapshot  %s old",
                    FormatDelay(account_age).c_str());
        ImGui::Text("Positions         %zu / %s old", app.positions.size(),
                    FormatDelay(positions_age).c_str());
        ImGui::Text("Order history     %s (%zu orders)",
                    app.orders_received_at_ms > 0 ? "LOADED" : "NOT LOADED",
                    app.orders.size());
        ImGui::Text("Exchange clock    %s old",
                    FormatDelay(clock_age).c_str());
        ImGui::Separator();
        ImGui::Text("Environment       %s",
                    app.active_paper ? "PAPER" : "LIVE");
        ImGui::Text("Account           %s",
                    app.has_account ? app.account.account_number.c_str()
                                    : "NONE");
        ImGui::Text("Alpaca status     %s",
                    app.has_account ? app.account.status.c_str() : "--");
        if (restrictions)
            ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.35f, 1.0f),
                               "TRADING RESTRICTION PRESENT");
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

float DrawMainMenuBar(App& app) {
    if (!ImGui::BeginMainMenuBar()) return 0.0f;

    const float title_control_height = ImGui::GetFrameHeight();
    if (ImGui::BeginMenu("Menu")) {
        const bool connected =
            app.connection_state == ConnectionState::AccountAuthenticated ||
            app.connection_state == ConnectionState::Streaming;
        const bool connecting =
            app.connection_state == ConnectionState::Connecting;
        const char* state_label = "OFFLINE";
        if (connecting)
            state_label = "CONNECTING";
        else if (app.connection_state == ConnectionState::AccountAuthenticated)
            state_label = "AUTHENTICATED";
        else if (app.connection_state == ConnectionState::Streaming)
            state_label = "STREAMING";
        else if (app.connection_state == ConnectionState::Error)
            state_label = "ERROR";
        std::string account_label = "Account    ";
        account_label += app.active_paper ? "PAPER" : "LIVE";
        account_label += " - ";
        account_label += state_label;
        if (ImGui::BeginMenu(account_label.c_str())) {
            ImGui::TextDisabled("ALPACA ACCOUNT");
            if (app.has_account)
                ImGui::TextUnformatted(app.account.account_number.c_str());
            else
                ImGui::TextDisabled("No authenticated account");
            ImGui::Separator();
            if (connecting) {
                ImGui::BeginDisabled();
                ImGui::MenuItem("Connecting...");
                ImGui::EndDisabled();
                if (ImGui::MenuItem("Cancel connection"))
                    DisconnectAccount(app);
            } else if (connected) {
                ImGui::BeginDisabled();
                ImGui::MenuItem("Connected", nullptr, true);
                ImGui::EndDisabled();
                if (ImGui::MenuItem(app.active_paper
                                        ? "Switch to live account"
                                        : "Switch to paper account"))
                    ConnectSavedAccount(app, !app.active_paper);
                if (ImGui::MenuItem("Disconnect")) DisconnectAccount(app);
            } else {
                if (ImGui::MenuItem("Connect paper account"))
                    ConnectSavedAccount(app, true);
                if (ImGui::MenuItem("Connect live account"))
                    ConnectSavedAccount(app, false);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Add")) {
            if (ImGui::BeginMenu("Chart")) {
                if (app.watchlist.empty())
                    ImGui::TextDisabled("Add a symbol to the watchlist first");
                for (const std::string& symbol : app.watchlist) {
                    auto chart = app.charts.find(symbol);
                    if (chart == app.charts.end()) continue;
                    if (ImGui::MenuItem(symbol.c_str())) {
                        chart->second.window_open = true;
                        RequestChartData(app, chart->second);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Order")) {
                app.show_order_entry = true;
                app.order_tickets.emplace_back();
            }
            if (ImGui::MenuItem("Quick order")) app.show_quick_order = true;
            if (ImGui::MenuItem("Bracket order")) app.show_oco_order = true;
            if (ImGui::MenuItem("Order management"))
                app.show_order_management = true;
            if (ImGui::MenuItem("Time & Sales"))
                app.show_time_sales = true;
            if (ImGui::MenuItem("Watchlist")) app.show_watchlist = true;
            if (ImGui::MenuItem("Account")) app.show_account = true;
            if (ImGui::MenuItem("Activity")) app.show_activity = true;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::BeginDisabled();
        ImGui::MenuItem("Lock workstation", "Coming later");
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Settings and credentials"))
            app.credentials_open = true;
        if (ImGui::MenuItem("Reset window layout"))
            app.workspace.ResetAll();
        if (ImGui::MenuItem("Screenshot")) {
            app.capture_path = DefaultScreenshotPath(app.database);
            app.capture_requested = true;
        }
        ImGui::EndMenu();
    }

    ImGui::SameLine(0, 4);
    DrawConnectionLight(app, title_control_height);

    const float right_edge = std::max(0.0f, ImGui::GetWindowWidth() - 8.0f);
    std::string account_identity;
    if (app.has_account) {
        std::ostringstream summary;
        summary << (app.active_paper ? "PAPER" : "LIVE") << "  "
                << app.account_alias << "  " << app.account.account_number
                << "   Equity $" << std::fixed << std::setprecision(2)
                << app.account.equity.ToDisplayDouble() << "   Cash $"
                << app.account.cash.ToDisplayDouble() << "   BP $"
                << app.account.buying_power.ToDisplayDouble();
        account_identity = summary.str();
    } else {
        account_identity = app.active_paper ? "PAPER" : "LIVE";
        account_identity += "  NO ACCOUNT";
    }
    const float identity_width =
        ImGui::CalcTextSize(account_identity.c_str()).x;
    const float status_x =
        std::max(ImGui::GetCursorPosX() + 12.0f,
                 right_edge - identity_width - 12.0f);
    ImGui::SameLine(status_x);
    const ImVec2 title_bar_position = ImGui::GetWindowPos();
    ImGui::PushClipRect(
        ImVec2(title_bar_position.x + status_x, title_bar_position.y),
        ImVec2(title_bar_position.x + right_edge,
               title_bar_position.y + ImGui::GetWindowHeight()),
        true);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(
        app.active_paper ? ImVec4(0.55f, 0.68f, 0.86f, 1)
                         : ImVec4(1.0f, 0.52f, 0.28f, 1),
        "%s", account_identity.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(account_identity.c_str());
        if (app.has_account) {
            ImGui::Separator();
            ImGui::TextWrapped(
                "Equity is the marked-to-market account value. Cash is the "
                "cash ledger balance and may be negative in a margin account. "
                "BP is Alpaca's current buying power for opening positions.");
            if (!app.account.multiplier.empty())
                ImGui::Text("Reported multiplier  %sx",
                            app.account.multiplier.c_str());
        }
        ImGui::EndTooltip();
    }
    ImGui::PopClipRect();

    const float height = ImGui::GetWindowHeight();
    ImGui::EndMainMenuBar();
    return height;
}

void ForgetSavedCredentials(App& app, bool paper) {
    std::string error;
    if (!CredentialStore::Delete(paper, error)) {
        AddMessage(app, error);
        return;
    }
    app.database.ClearLastConnectedAccount(paper);
    if (app.active_paper == paper) DisconnectAccount(app);
    ClearCredentialInputs(app);
    AddMessage(app, std::string("Forgot saved ") +
                        (paper ? "paper" : "live") +
                        " credentials from Windows Credential Manager");
}

void DrawCredentials(App& app, SDL_Window* window) {
    if (!app.credentials_open) return;
    ImGui::OpenPopup("Settings");
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Settings", &app.credentials_open,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoMove)) {
        ImGui::SeparatorText("User interface");
        bool presentation_changed = false;
        if (ImGui::Checkbox("Vertical sync", &app.vsync_requested)) {
            app.database.SaveAppSetting(
                "ui.vsync", app.vsync_requested ? "1" : "0");
            presentation_changed = true;
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Maximum FPS (0 = unlimited)",
                            &app.maximum_frame_rate, 30, 120)) {
            app.maximum_frame_rate =
                std::clamp(app.maximum_frame_rate, 0, 10'000);
            app.database.SaveAppSetting(
                "ui.maximum_fps",
                std::to_string(app.maximum_frame_rate));
            presentation_changed = true;
        }
        int snap_pixels = app.workspace.SnapPixels();
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Window snap (pixels)", &snap_pixels, 1, 10)) {
            app.workspace.SetSnapPixels(snap_pixels);
            app.database.SaveAppSetting(
                "ui.window_snap_pixels",
                std::to_string(app.workspace.SnapPixels()));
        }
        if (presentation_changed)
            ApplyPresentationSettings(app, window, true);
        ImGui::TextDisabled(
            "Default: VSync on with a 120 FPS fallback cap.");
        ImGui::TextDisabled(
            "Window positions and sizes align from the canvas origin (0,0).");

        ImGui::SeparatorText("Alpaca connection");
        ImGui::TextWrapped(
            "Keys are stored in Windows Credential Manager, never in the "
            "workspace database.");
        ImGui::Checkbox("Paper account", &app.credential_paper);
        ImGui::InputText("API key", app.key.data(), app.key.size());
        ImGui::InputText("Secret", app.secret.data(), app.secret.size(),
                         ImGuiInputTextFlags_Password);
        if (ImGui::Button("Save and connect", ImVec2(180, 0))) {
            AlpacaCredentials credentials{app.key.data(), app.secret.data(),
                                          app.credential_paper};
            std::string error;
            if (credentials.key.empty() || credentials.secret.empty()) {
                AddMessage(app, "Both API key and secret are required");
            } else if (!CredentialStore::Save(credentials, error)) {
                AddMessage(app, error);
            } else {
                ClearCredentialInputs(app);
                app.has_account = false;
                app.positions.clear();
                app.orders.clear();
                app.positions_received_at_ms = 0;
                app.orders_received_at_ms = 0;
                app.active_paper = credentials.paper;
                app.connection_state = ConnectionState::Connecting;
                app.market_latency_ms = -1;
                app.last_market_received_at_ms = 0;
                app.market_subscription_active = false;
                app.account_stream_latency_ms = -1;
                app.last_account_stream_event_at_ms = 0;
                app.account_event_latency_ms = -1;
                app.account_stream_failed = false;
                const auto core_result = app.application.Connect({
                    .environment =
                        credentials.paper
                            ? tradebox::core::AccountEnvironment::Paper
                            : tradebox::core::AccountEnvironment::Live,
                    .api_key = credentials.key,
                    .api_secret = credentials.secret,
                    .market_symbols = app.watchlist,
                });
                if (!core_result || !core_result->accepted) {
                    AddMessage(app, !core_result
                                        ? core_result.error().message
                                        : core_result->message);
                } else {
                    app.application.RefreshAssetCatalog();
                    app.credentials_open = false;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ClearCredentialInputs(app);
            app.credentials_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SeparatorText("Saved credentials");
        if (ImGui::Button("Forget saved paper"))
            ImGui::OpenPopup("Forget paper credentials?");
        ImGui::SameLine();
        if (ImGui::Button("Forget saved live"))
            ImGui::OpenPopup("Forget live credentials?");

        if (ImGui::BeginPopupModal("Forget paper credentials?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "Remove the saved paper API key and secret from Windows "
                "Credential Manager?");
            if (ImGui::Button("Forget paper", ImVec2(130, 0))) {
                ForgetSavedCredentials(app, true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal("Forget live credentials?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "Remove the saved live API key and secret from Windows "
                "Credential Manager?");
            if (ImGui::Button("Forget live", ImVec2(130, 0))) {
                ForgetSavedCredentials(app, false);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::EndPopup();
    }
    if (!app.credentials_open) ClearCredentialInputs(app);
}

void AddSymbol(App& app, const std::string& symbol) {
    if (symbol.empty()) return;
    if (std::find(app.watchlist.begin(), app.watchlist.end(), symbol) !=
        app.watchlist.end())
        return;
    auto next_watchlist = app.watchlist;
    next_watchlist.push_back(symbol);
    if (const auto saved = app.database.SaveWatchlist(next_watchlist);
        !saved) {
        AddMessage(app, "Could not save watchlist: " + saved.error());
        return;
    }
    app.watchlist = std::move(next_watchlist);
    Chart chart{symbol};
    chart.timeframe = "1Min";
    auto [chart_it, inserted] = app.charts.emplace(symbol, std::move(chart));
    app.application.RefreshMarketSymbols(app.watchlist);
    if (inserted) RequestChartData(app, chart_it->second);
}

void RequestChartData(App& app, Chart& chart) {
    const auto now = std::chrono::system_clock::now();
    const auto start = chart.timeframe == "1Min"
                           ? now - std::chrono::hours(24 * 7)
                           : now - std::chrono::hours(24 * 365 * 6);
    const auto to_ns = [](auto point) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            point.time_since_epoch())
            .count();
    };
    chart.requested_range = {to_ns(start), to_ns(now)};
    app.application.RequestMarketHistory(
        chart.symbol, chart.timeframe, chart.requested_range);
}

void OpenChart(App& app, Chart& chart) {
    chart.window_open = true;
    chart.view_state = tradebox::ui::ChartViewDataState::Loading;
    chart.data_message.clear();
    RequestChartData(app, chart);
}

void RemoveSymbol(App& app, std::size_t index) {
    if (index >= app.watchlist.size()) return;
    auto next_watchlist = app.watchlist;
    next_watchlist.erase(
        next_watchlist.begin() + static_cast<std::ptrdiff_t>(index));
    if (const auto saved = app.database.SaveWatchlist(next_watchlist);
        !saved) {
        AddMessage(app, "Could not save watchlist: " + saved.error());
        return;
    }
    app.charts.erase(app.watchlist[index]);
    app.watchlist = std::move(next_watchlist);
    app.application.RefreshMarketSymbols(app.watchlist);
}

void FormatDate(std::int64_t timestamp_ms, char* output, std::size_t size);

void DrawWatchlist(App& app) {
    if (!app.show_watchlist) return;
    app.workspace.ConstrainNextWindowSize(
        ImVec2(320, 160), ImGui::GetMainViewport()->WorkSize);
    if (!app.workspace.BeginWindow("tool.watchlist", "WATCHLIST",
                                   &app.show_watchlist,
                                   ImVec2(10.0f, 440.0f),
                                   ImVec2(420.0f, 300.0f))) {
        app.workspace.EndWindow("tool.watchlist");
        return;
    }
    ImGui::SetNextItemWidth(180);
    const bool submitted = ImGui::InputText(
        "##symbol", app.new_symbol.data(), app.new_symbol.size(),
        ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string asset_query =
        NormalizeSymbol(app.new_symbol.data());
    if (asset_query != app.watchlist_asset_query) {
        app.watchlist_asset_query = asset_query;
        app.asset_cursor = 0;
        app.asset_matches =
            asset_query.empty()
                ? std::vector<tradebox::core::TradableAsset>{}
                : tradebox::core::SearchTradableAssets(
                      app.asset_catalog, asset_query, 5);
    }
    if (ImGui::IsItemActive() && !app.asset_matches.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            app.asset_cursor = (app.asset_cursor + 1) % static_cast<int>(app.asset_matches.size());
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            app.asset_cursor = (app.asset_cursor + static_cast<int>(app.asset_matches.size()) - 1) % static_cast<int>(app.asset_matches.size());
        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            std::snprintf(app.new_symbol.data(), app.new_symbol.size(), "%s",
                          app.asset_matches[static_cast<std::size_t>(app.asset_cursor)].symbol.c_str());
            AddSymbol(app, app.new_symbol.data());
            app.new_symbol.fill('\0');
            app.asset_matches.clear();
        }
    }
    if ((ImGui::IsItemHovered() || ImGui::IsItemActive()) && !app.asset_matches.empty()) {
        ImGui::BeginTooltip();
        for (std::size_t index = 0; index < app.asset_matches.size(); ++index) {
            const auto& asset = app.asset_matches[index];
            if (static_cast<int>(index) == app.asset_cursor) ImGui::TextColored(ImVec4(.3f,.85f,1,1), "> %s", asset.symbol.c_str());
            else ImGui::Text("  %s", asset.symbol.c_str());
            ImGui::SameLine(100); ImGui::TextDisabled("%s | %s", asset.name.c_str(), asset.exchange.c_str());
        }
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    if ((ImGui::Button("+ Add") || submitted)) {
        AddSymbol(app, NormalizeSymbol(app.new_symbol.data()));
        app.new_symbol.fill('\0');
    }
    ImGui::Separator();
    std::size_t remove = app.watchlist.size();
    if (ImGui::BeginTable("watchlist-prices", 4,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_Hideable |
                              ImGuiTableFlags_Reorderable |
                              ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthStretch,
                                1.0f);
        ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthStretch,
                                1.0f);
        ImGui::TableSetupColumn("Since close",
                                ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##remove",
                                ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < app.watchlist.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            auto found = app.charts.find(app.watchlist[i]);
            Chart* chart =
                found != app.charts.end() ? &found->second : nullptr;
            const bool selected = app.selected_symbol == app.watchlist[i];
            if (ImGui::Selectable(app.watchlist[i].c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns))
                SelectTimeSalesSymbol(app, app.watchlist[i], true);
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && chart)
                OpenChart(app, *chart);
            if (ImGui::BeginPopupContextItem("watch-symbol-menu")) {
                if (ImGui::MenuItem("Open chart")) {
                    SelectTimeSalesSymbol(app, app.watchlist[i], true);
                    if (chart) OpenChart(app, *chart);
                }
                if (ImGui::MenuItem("Open trade order")) {
                    SelectTimeSalesSymbol(app, app.watchlist[i], true);
                    app.show_order_entry = true;
                    if (app.order_tickets.empty())
                        app.order_tickets.emplace_back();
                }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            const auto market = app.market_views.find(app.watchlist[i]);
            const bool has_live_trade =
                market != app.market_views.end() &&
                !market->second.trades.empty();
            const double live_trade =
                has_live_trade
                    ? market->second.trades.front()
                          .price.ToDisplayDouble()
                    : 0;
            if (has_live_trade)
                ImGui::Text("%.2f", live_trade);
            else if (chart && !chart->bars.empty()) {
                ImGui::TextDisabled("%.2f", chart->bars.back().close);
                if (ImGui::IsItemHovered()) {
                    char date[32];
                    FormatDate(chart->bars.back().timestamp_ms, date,
                               sizeof(date));
                    ImGui::SetTooltip("Last cached daily close (%s)", date);
                }
            } else
                ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            const double previous_close =
                chart && chart->core_snapshot.previous_session_close
                    ? chart->core_snapshot.previous_session_close
                          ->ToDisplayDouble()
                    : 0;
            if (has_live_trade && previous_close > 0) {
                const double change =
                    (live_trade / previous_close - 1.0) * 100.0;
                ImGui::TextColored(
                    change >= 0 ? ImVec4(0.25f, 0.9f, 0.58f, 1)
                                : ImVec4(0.96f, 0.38f, 0.43f, 1),
                    "%+.2f%%", change);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Previous close %.2f", previous_close);
            } else {
                ImGui::TextDisabled("--");
            }
            ImGui::TableNextColumn();
        if (ImGui::SmallButton("x")) remove = i;
        ImGui::PopID();
    }
        ImGui::EndTable();
    }
    if (remove < app.watchlist.size()) RemoveSymbol(app, remove);
    app.workspace.EndWindow("tool.watchlist");
}

void FormatDate(std::int64_t timestamp_ms, char* output, std::size_t size) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    std::strftime(output, size, "%Y-%m-%d", &utc);
}

void DrawChartCanvas(Chart& chart) {
    const std::string plot_id = "chart-plot-" + chart.symbol + "-" +
                                chart.timeframe;
    tradebox::ui::RenderChartView(plot_id, chart.timeframe,
                                  chart.view_series, chart.visible_bars,
                                  chart.reset_x_range,
                                  chart.view_state, chart.view_options);
}

void DrawCharts(App& app) {
    for (const std::string& symbol : app.watchlist) {
        auto found = app.charts.find(symbol);
        if (found == app.charts.end()) continue;
        Chart& chart = found->second;
        if (!chart.window_open) continue;
        const std::string title = symbol + " - " + chart.timeframe + "###chart-" + symbol;
        app.workspace.ConstrainNextWindowSize();
        if (!app.workspace.BeginWindow(
                "chart." + symbol, title, &chart.window_open,
                ImVec2(1500.0f, 780.0f), ImVec2(640.0f, 390.0f))) {
            app.workspace.EndWindow("chart." + symbol);
            continue;
        }
        // Compact chart toolbar.  The range controls intentionally act on
        // bars, not pixels, so a zoom keeps the right-hand (latest) time fixed.
        const auto select_timeframe = [&app, &chart](const char* label,
                                                      const char* timeframe) {
            if (ImGui::Button(label)) {
                chart.timeframe = timeframe;
                chart.core_snapshot = {};
                chart.requested_range = {};
                chart.bars.clear();
                chart.view_series.bars.clear();
                chart.view_state = tradebox::ui::ChartViewDataState::Loading;
                chart.history_loaded = false;
                chart.reset_x_range = true;
                RequestChartData(app, chart);
            }
        };
        select_timeframe("1m", "1Min");
        ImGui::SameLine();
        select_timeframe("1h", "1Hour");
        ImGui::SameLine();
        select_timeframe("1D", "1Day");
        ImGui::SameLine();
        if (ImGui::Button("-##zoom-out")) {
            chart.visible_bars = std::min(2'000, chart.visible_bars * 2);
            chart.reset_x_range = true;
        }
        ImGui::SetItemTooltip("Zoom out: show earlier bars while keeping the latest time fixed");
        ImGui::SameLine();
        if (ImGui::Button("+##zoom-in")) {
            chart.visible_bars = std::max(30, chart.visible_bars / 2);
            chart.reset_x_range = true;
        }
        ImGui::SetItemTooltip("Zoom in: keep the latest time fixed");
        ImGui::SameLine();
        if (ImGui::Button("Reset##chart-range")) {
            chart.visible_bars = 120;
            chart.reset_x_range = true;
        }
        ImGui::SetItemTooltip("Reset chart range");
        ImGui::SameLine();
        if (ImGui::Button(chart.view_options.show_volume ? "Vol" : "Vol off"))
            chart.view_options.show_volume = !chart.view_options.show_volume;
        ImGui::SetItemTooltip("Show or hide volume");
        ImGui::SameLine();
        if (ImGui::Button(chart.view_options.show_crosshair ? "Cross" : "Cross off"))
            chart.view_options.show_crosshair = !chart.view_options.show_crosshair;
        ImGui::SetItemTooltip("Show or hide crosshair");
        ImGui::SameLine();
        ImGui::TextDisabled("%s  |  %d bars", chart.timeframe.c_str(),
                            chart.visible_bars);
        if (!chart.view_series.bars.empty()) {
            const Bar& latest = chart.view_series.bars.back();
            const ImVec4 color = latest.close >= latest.open
                                     ? ImVec4(0.25f, 0.9f, 0.58f, 1)
                                     : ImVec4(0.96f, 0.38f, 0.43f, 1);
            ImGui::TextDisabled("O");
            ImGui::SameLine();
            ImGui::Text("%.2f", latest.open);
            ImGui::SameLine();
            ImGui::TextDisabled("H");
            ImGui::SameLine();
            ImGui::Text("%.2f", latest.high);
            ImGui::SameLine();
            ImGui::TextDisabled("L");
            ImGui::SameLine();
            ImGui::Text("%.2f", latest.low);
            ImGui::SameLine();
            ImGui::TextDisabled("C");
            ImGui::SameLine();
            ImGui::TextColored(color, "%.2f", latest.close);
            ImGui::SameLine();
            ImGui::TextDisabled("%zu bars | wheel zoom | drag pan",
                                chart.view_series.bars.size());
        } else {
            ImGui::TextDisabled("%s / IEX", chart.timeframe.c_str());
        }
        if (!chart.data_message.empty())
            ImGui::TextWrapped("%s", chart.data_message.c_str());
        DrawChartCanvas(chart);
        app.workspace.EndWindow("chart." + symbol);
    }
}

void DrawPositionsContent(App& app) {
    if (!app.has_account) {
        ImGui::TextDisabled("Connect an Alpaca account to load positions.");
        return;
    }
    const bool live_valuation = !app.positions.empty() &&
        std::ranges::all_of(
        app.positions, [](const tradebox::core::PositionState& position) {
            return position.valuation_current &&
                   position.valuation_from_market_stream;
        });
    ImGui::TextDisabled("%zu open positions | %s", app.positions.size(),
                        live_valuation ? "LIVE LAST-TRADE VALUATION"
                                       : "WAITING FOR LIVE PRICES");
    if (ImGui::BeginTable(
            "positions-table", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable |
                ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Symbol");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Qty");
        ImGui::TableSetupColumn("Average");
        ImGui::TableSetupColumn("Last");
        ImGui::TableSetupColumn("Market value");
        ImGui::TableSetupColumn("Total P/L");
        ImGui::TableSetupColumn("Today P/L");
        ImGui::TableHeadersRow();
        for (const tradebox::core::PositionState& position :
             app.positions) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(position.symbol.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(position.side.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(position.qty.ToString().c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(
                position.avg_entry_price.ToString().c_str());
            ImGui::TableNextColumn();
            if (position.valuation_current)
                ImGui::TextUnformatted(
                    position.current_price.ToString().c_str());
            else
                ImGui::TextDisabled("-- stale");
            ImGui::TableNextColumn();
            if (position.valuation_current)
                ImGui::Text(
                    "$%s",
                    position.market_value.ToString().c_str());
            else
                ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            if (position.valuation_current)
                ImGui::TextColored(
                    position.unrealized_pl >=
                            tradebox::core::Decimal::Zero()
                        ? ImVec4(0.25f, 0.9f, 0.58f, 1)
                        : ImVec4(0.96f, 0.38f, 0.43f, 1),
                    "%+.2f  (%+.2f%%)",
                    position.unrealized_pl.ToDisplayDouble(),
                    position.unrealized_plpc.ToDisplayDouble() *
                        100.0);
            else
                ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            if (position.valuation_current)
                ImGui::TextColored(
                    position.unrealized_intraday_pl >=
                            tradebox::core::Decimal::Zero()
                        ? ImVec4(0.25f, 0.9f, 0.58f, 1)
                        : ImVec4(0.96f, 0.38f, 0.43f, 1),
                    "%+.2f  (%+.2f%%)",
                    position.unrealized_intraday_pl
                        .ToDisplayDouble(),
                    position.unrealized_intraday_plpc
                            .ToDisplayDouble() *
                        100.0);
            else
                ImGui::TextDisabled("--");
        }
        ImGui::EndTable();
    }
}

void DrawOrderManagement(App& app) {
    if (!app.show_order_management) return;
    app.workspace.ConstrainNextWindowSize();
    if (!app.workspace.BeginWindow("tool.order_management", "ORDER MANAGEMENT",
                                   &app.show_order_management,
                                   ImVec2(440.0f, 700.0f),
                                   ImVec2(760.0f, 290.0f))) {
        app.workspace.EndWindow("tool.order_management");
        return;
    }
    if (!app.has_account) {
        ImGui::TextDisabled("Connect an account to manage open orders.");
        app.workspace.EndWindow("tool.order_management");
        return;
    }

    ImGui::TextDisabled("WORKING ORDERS");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputTextWithHint("Symbol filter", "All symbols",
                             app.order_management_symbol.data(),
                             app.order_management_symbol.size(),
                             ImGuiInputTextFlags_CharsUppercase);
    const std::string filter =
        NormalizeSymbol(app.order_management_symbol.data());
    std::vector<const tradebox::core::OrderState*> working;
    for (const auto& order : app.orders) {
        const auto projection = tradebox::core::ProjectOrder(order);
        if (projection.capabilities.cancelable &&
            (filter.empty() || order.symbol == filter))
            working.push_back(&order);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu working", working.size());
    ImGui::Separator();

    if (ImGui::BeginTable(
            "working-orders", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable |
                ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Symbol");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Quantity");
        ImGui::TableSetupColumn("Limit / stop");
        ImGui::TableSetupColumn("Filled");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        for (const auto* order : working) {
            ImGui::PushID(order->id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(order->symbol.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(order->side.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(order->type.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(order->qty
                ? order->qty->ToString().c_str() : "--");
            ImGui::TableNextColumn();
            if (order->limit_price)
                ImGui::Text("L %s", order->limit_price->ToString().c_str());
            else if (order->stop_price)
                ImGui::Text("S %s", order->stop_price->ToString().c_str());
            else
                ImGui::TextDisabled("Market");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(order->filled_qty.ToString().c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(
                UiOrderStateLabel(UiOrderStateFromCore(*order, app.core_view)).c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(app.command_in_flight);
            if (ImGui::Button("CANCEL")) {
                const auto snapshot = app.application.Snapshot();
                const std::string request_id = NextRequestId(app);
                SubmitUiCommand(app, tradebox::core::CancelOrderCommand{
                    .context = {.request_id = request_id,
                                .source = "order-management",
                                .account_id = snapshot.account
                                    ? snapshot.account->id : "",
                                .environment = snapshot.environment,
                                .generation = snapshot.generation},
                    .order_id = order->id}, request_id);
                app.command_status = "pending cancel " + order->symbol;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!app.command_status.empty())
        ImGui::TextDisabled("%s", app.command_status.c_str());
    app.workspace.EndWindow("tool.order_management");
}

void DrawOrdersContent(App& app) {
    if (!app.has_account) {
        ImGui::TextDisabled("Connect an Alpaca account to load order history.");
        return;
    }
    const tradebox::core::SafetyStatus safety =
        app.core_view.safety_status;
    if (safety == tradebox::core::SafetyStatus::Live &&
        app.orders_received_at_ms > 0) {
        ImGui::TextDisabled("%zu orders | LIVE via trade_updates",
                            app.orders.size());
    } else if (safety == tradebox::core::SafetyStatus::Reconciling ||
               safety == tradebox::core::SafetyStatus::SnapshotLoading ||
               safety == tradebox::core::SafetyStatus::Connecting) {
        ImGui::TextColored(
            ImVec4(0.96f, 0.72f, 0.30f, 1.0f),
            "%zu orders | RECONCILING", app.orders.size());
    } else {
        ImGui::TextColored(
            ImVec4(0.96f, 0.56f, 0.30f, 1.0f),
            "%zu orders | STALE %s old",
            app.orders.size(),
            FormatDelay(app.orders_received_at_ms > 0
                            ? SystemNowMs() - app.orders_received_at_ms
                            : -1)
                .c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("Active", &app.show_active_orders);
    ImGui::SameLine();
    ImGui::Checkbox("Filled", &app.show_filled_orders);

    std::vector<const tradebox::core::OrderState*> visible_orders;
    visible_orders.reserve(app.orders.size());
    for (const auto& order : app.orders) {
        const auto projection = tradebox::core::ProjectOrder(order);
        const bool active = projection.lifecycle ==
                                tradebox::core::OrderLifecycleState::Pending ||
                            projection.lifecycle ==
                                tradebox::core::OrderLifecycleState::Accepted;
        if ((app.show_active_orders && active) ||
            (app.show_filled_orders &&
             projection.lifecycle == tradebox::core::OrderLifecycleState::Filled))
            visible_orders.push_back(&order);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu shown", visible_orders.size());
    if (ImGui::BeginTable(
            "orders-table", 10,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable |
                ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Submitted");
        ImGui::TableSetupColumn("Symbol");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Qty");
        ImGui::TableSetupColumn("Filled");
        ImGui::TableSetupColumn("Average fill");
        ImGui::TableSetupColumn("TIF");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible_orders.size()));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart;
                 index < clipper.DisplayEnd; ++index) {
                const tradebox::core::OrderState& order =
                    *visible_orders[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string submitted =
                    order.submitted_at.size() > 19
                        ? order.submitted_at.substr(0, 19)
                        : order.submitted_at;
                ImGui::TextUnformatted(submitted.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(order.symbol.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(order.side.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(order.type.c_str());
                ImGui::TableNextColumn();
                if (order.qty)
                    ImGui::TextUnformatted(order.qty->ToString().c_str());
                else if (order.notional)
                    ImGui::Text("$%s",
                                order.notional->ToString().c_str());
                else
                    ImGui::TextDisabled("--");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(order.filled_qty.ToString().c_str());
                ImGui::TableNextColumn();
                if (order.filled_avg_price)
                    ImGui::TextUnformatted(
                        order.filled_avg_price->ToString().c_str());
                else
                    ImGui::TextDisabled("--");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(order.time_in_force.c_str());
                ImGui::TableNextColumn();
                const UiOrderState ui_state = UiOrderStateFromCore(order, app.core_view);
                ImGui::TextUnformatted(UiOrderStateLabel(ui_state).c_str());
                ImGui::TableNextColumn();
                const bool cancelable =
                    tradebox::core::ProjectOrder(order).capabilities.cancelable;
                ImGui::PushID(order.id.c_str());
                ImGui::BeginDisabled(!cancelable || app.command_in_flight);
                if (ImGui::SmallButton("Cancel")) {
                    const auto snapshot = app.application.Snapshot();
                    const std::string request_id = NextRequestId(app);
                    SubmitUiCommand(app, tradebox::core::CancelOrderCommand{
                        .context = {.request_id = request_id, .source = "gui",
                                    .account_id = snapshot.account ? snapshot.account->id : "",
                                    .environment = snapshot.environment,
                                    .generation = snapshot.generation},
                        .order_id = order.id}, request_id);
                    app.command_status = "pending cancel";
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

const char* EventSeverityLabel(EventSeverity severity) {
    switch (severity) {
    case EventSeverity::Info: return "INFO";
    case EventSeverity::Warning: return "WARNING";
    case EventSeverity::Error: return "ERROR";
    case EventSeverity::Auto: return "INFO";
    }
    return "INFO";
}

ImVec4 EventSeverityColor(EventSeverity severity) {
    switch (severity) {
    case EventSeverity::Warning: return ImVec4(0.96f, 0.72f, 0.30f, 1.0f);
    case EventSeverity::Error: return ImVec4(0.96f, 0.38f, 0.43f, 1.0f);
    case EventSeverity::Auto:
    case EventSeverity::Info: return ImVec4(0.48f, 0.67f, 0.88f, 1.0f);
    }
    return ImVec4(0.48f, 0.67f, 0.88f, 1.0f);
}

std::string FormatEventTimestamp(std::int64_t timestamp_ms) {
    const std::time_t seconds =
        static_cast<std::time_t>(timestamp_ms / 1'000);
    std::tm local{};
    localtime_s(&local, &seconds);
    std::ostringstream result;
    result << std::put_time(&local, "%H:%M:%S");
    return result.str();
}

std::string CopyableEventLog(const std::vector<EventLogEntry>& entries) {
    std::ostringstream text;
    for (const EventLogEntry& entry : entries) {
        text << FormatEventTimestamp(entry.recorded_at_ms) << "  "
             << EventSeverityLabel(entry.severity) << "  " << entry.text
             << '\n';
    }
    return text.str();
}

void DrawEventLogContent(App& app) {
    ImGui::TextDisabled("Newest first. Copy exact broker and history details for troubleshooting.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy all")) {
        const std::string copy = CopyableEventLog(app.messages);
        ImGui::SetClipboardText(copy.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu retained", app.messages.size());
    ImGui::Separator();
    if (!ImGui::BeginTable(
            "event-log", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 74.0f);
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 44.0f);
    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (std::size_t index = 0; index < app.messages.size(); ++index) {
        const EventLogEntry& entry = app.messages[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const std::string timestamp = FormatEventTimestamp(entry.recorded_at_ms);
        ImGui::TextUnformatted(timestamp.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(EventSeverityColor(entry.severity), "%s",
                           EventSeverityLabel(entry.severity));
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Copy"))
            ImGui::SetClipboardText(entry.text.c_str());
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", entry.text.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Use Copy to place this exact text on the clipboard.");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void DrawActivity(App& app) {
    if (!app.show_activity) return;
    app.workspace.ConstrainNextWindowSize(ImVec2(620.0f, 360.0f));
    if (!app.workspace.BeginWindow("tool.activity", "ACTIVITY",
                                   &app.show_activity,
                                   ImVec2(440.0f, 10.0f),
                                   ImVec2(980.0f, 680.0f))) {
        app.workspace.EndWindow("tool.activity");
        return;
    }
    if (ImGui::BeginTabBar("activity-tabs", ImGuiTabBarFlags_Reorderable)) {
        if (ImGui::BeginTabItem("Positions")) {
            DrawPositionsContent(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Orders")) {
            DrawOrdersContent(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Events")) {
            DrawEventLogContent(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    app.workspace.EndWindow("tool.activity");
}

int RunApplication(const LaunchOptions& options) {
    Database database;
    std::string database_error;
    if (!database.Open(database_error)) {
        MessageBoxA(nullptr, database_error.c_str(), "Trade Box database error",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return 1;
    WindowPlacement placement = database.LoadWindowPlacement();
    placement.width = std::max(placement.width, 640);
    placement.height = std::max(placement.height, 480);
    int initial_x = SDL_WINDOWPOS_CENTERED;
    int initial_y = SDL_WINDOWPOS_CENTERED;
    const bool placement_valid =
        placement.exists && PlacementTouchesDisplay(placement);
    if (placement_valid) {
        initial_x = placement.x;
        initial_y = placement.y;
    } else {
        placement.maximized = false;
    }
    SDL_PropertiesID window_properties = SDL_CreateProperties();
    SDL_SetStringProperty(window_properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                          "Trade Box");
    SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                          initial_x);
    SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                          initial_y);
    SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
                          placement.width);
    SDL_SetNumberProperty(window_properties,
                          SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, placement.height);
    SDL_SetBooleanProperty(window_properties,
                           SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(window_properties,
                           SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, false);
    // Do not expose the native window until DirectX has presented real UI
    // content. Windows otherwise paints a temporary blank client surface
    // while database/UI initialization is still in progress.
    SDL_SetBooleanProperty(window_properties,
                           SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    SDL_SetBooleanProperty(
        window_properties,
        SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_Window* window = SDL_CreateWindowWithProperties(window_properties);
    SDL_DestroyProperties(window_properties);
    if (!window) {
        SDL_Quit();
        return 1;
    }
    if (placement.maximized) SDL_MaximizeWindow(window);
    Dx11Renderer renderer;
    std::string renderer_error;
    if (!CreateDx11Renderer(window, renderer, renderer_error)) {
        MessageBoxA(nullptr, renderer_error.c_str(), "Trade Box DirectX error",
                    MB_OK | MB_ICONERROR);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    // Chart wheel zoom is applied in RenderChartView with the latest time as
    // its anchor. Keep ImPlot's mouse-centred zoom available behind Ctrl.
    ImPlot::GetInputMap().ZoomMod = ImGuiMod_Ctrl;
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    // Recoverable ImGui diagnostics must never take mouse focus away from a
    // trading surface. Application and broker errors are reported in EVENT LOG.
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    const std::filesystem::path font_path = ApplicationAssetPath(
        "assets/fonts/B612-Regular.ttf");
    if (io.Fonts->AddFontFromFileTTF(font_path.string().c_str(), 12.0f) ==
        nullptr) {
        // Keep a usable application surface if an incomplete manual install
        // omits the packaged font assets.
        io.Fonts->AddFontDefaultVector();
    }
    ImGui::GetStyle().FontSizeBase = 12.0f;
    ImGui::GetStyle().FontScaleDpi = SDL_GetWindowDisplayScale(window);
    // Layout v7 adds the grouped Activity surface. Older layout files remain
    // available for manual recovery.
    const std::string ini_path =
        (database.DataDirectory() / "workspace-layout-v7.ini").string();
    io.IniFilename = ini_path.c_str();
    ImGui_ImplSDL3_InitForD3D(window);
    ImGui_ImplDX11_Init(renderer.device.Get(), renderer.context.Get());

    App app(database);
    LoadWindowVisibility(app);
    app.ui_scale.CaptureBaseline();
    if (const auto value = database.LoadAppSetting("ui.scale"))
        app.ui_scale.SetScale(std::strtof(value->c_str(), nullptr));
    app.workspace.SetUiScale(app.ui_scale.Scale());
    if (const auto value = database.LoadAppSetting("ui.window_snap_pixels"))
        app.workspace.SetSnapPixels(std::atoi(value->c_str()));
    if (app.show_order_entry) app.order_tickets.emplace_back();
    AddMessage(app, "DirectX 11 renderer initialized");
    if (const auto value = database.LoadAppSetting("ui.vsync"))
        app.vsync_requested = *value == "1";
    if (const auto value = database.LoadAppSetting("ui.maximum_fps"))
        app.maximum_frame_rate =
            std::clamp(std::atoi(value->c_str()), 0, 10'000);
    ApplyPresentationSettings(app, window, false);
    const Uint64 launched_at = SDL_GetTicks();
    if (options.capture_and_exit) {
        app.capture_requested = true;
        app.capture_path = options.capture_path;
        app.exit_after_capture = true;
        app.frames_before_capture = 3;
    }
    app.watchlist = database.LoadWatchlist();
    app.asset_catalog = {
        {"AMD", "Advanced Micro Devices", "NASDAQ", true, true, true, true, 45'000'000, 7'000'000'000, SystemNowMs()},
        {"MSFT", "Microsoft Corporation", "NASDAQ", true, true, true, true, 22'000'000, 9'000'000'000, SystemNowMs()},
        {"AAPL", "Apple Inc.", "NASDAQ", true, true, true, true, 52'000'000, 11'000'000'000, SystemNowMs()},
        {"QQQ", "Invesco QQQ Trust", "NASDAQ", true, true, true, true, 31'000'000, 15'000'000'000, SystemNowMs()},
    };
    if (const auto cached_assets = database.LoadAssetCatalog(); !cached_assets.empty())
        app.asset_catalog = cached_assets;
    if (app.watchlist.empty()) {
        app.watchlist = {"AMD", "MSFT", "AAPL", "QQQ"};
    } else {
        for (const char* symbol : {"AMD", "MSFT", "AAPL", "QQQ"}) {
            if (app.watchlist.size() >= 4) break;
            if (std::find(app.watchlist.begin(), app.watchlist.end(), symbol) ==
                app.watchlist.end())
                app.watchlist.emplace_back(symbol);
        }
    }
    if (app.watchlist.size() >= 4) {
        if (const auto saved = database.SaveWatchlist(app.watchlist);
            !saved)
            AddMessage(app, "Could not save watchlist: " + saved.error());
    }
    for (const std::string& symbol : app.watchlist) {
        Chart chart{symbol};
        app.charts.emplace(symbol, std::move(chart));
    }
    RestoreOpenCharts(app);
    AddMessage(app, "Workspace loaded from " +
                        database.DataDirectory().string());
    if (options.connect_paper) {
        ConnectSavedAccount(app, true);
    } else if (const std::optional<bool> previous =
                   database.LoadLastConnectedPaper()) {
        ConnectSavedAccount(app, *previous);
    }

    int normal_x = placement.x, normal_y = placement.y,
        normal_width = placement.width, normal_height = placement.height;
    if (!placement_valid) {
        SDL_GetWindowPosition(window, &normal_x, &normal_y);
        SDL_GetWindowSize(window, &normal_width, &normal_height);
    }
    bool remember_maximized = placement.maximized;
    bool placement_dirty = false;
    Uint64 placement_changed_at = 0;
    bool window_shown = false;
    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                done = true;
            }
            if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
                ApplyPresentationSettings(app, window, true);
                // Re-arm after the new display has presented real frames.
                app.presentation_rearm_frames = 2;
            }
            if (event.type == SDL_EVENT_WINDOW_MAXIMIZED) {
                remember_maximized = true;
                placement_dirty = true;
                placement_changed_at = SDL_GetTicks();
            } else if (event.type == SDL_EVENT_WINDOW_RESTORED) {
                remember_maximized = false;
                placement_dirty = true;
                placement_changed_at = SDL_GetTicks();
            } else if (event.type == SDL_EVENT_WINDOW_MOVED ||
                       event.type == SDL_EVENT_WINDOW_RESIZED ||
                       event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                const SDL_DisplayID current_display =
                    SDL_GetDisplayForWindow(window);
                if (current_display != 0 &&
                    current_display != app.presentation_display) {
                    ApplyPresentationSettings(app, window, true);
                    app.presentation_rearm_frames = 2;
                }
                const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
                if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) ==
                    0) {
                    SDL_GetWindowPosition(window, &normal_x, &normal_y);
                    SDL_GetWindowSize(window, &normal_width, &normal_height);
                    placement_dirty = true;
                    placement_changed_at = SDL_GetTicks();
                }
            }
        }
        if (placement_dirty &&
            SDL_GetTicks() - placement_changed_at >= 500) {
            database.SaveWindowPlacement(
                {normal_x, normal_y, normal_width, normal_height,
                 remember_maximized, true});
            placement_dirty = false;
        }
        if (options.run_for_ms > 0 &&
            SDL_GetTicks() - launched_at >=
                static_cast<Uint64>(options.run_for_ms))
            done = true;
        DrainEvents(app);
        RefreshUiSnapshot(app);
        const Uint64 storage_now = SDL_GetTicks();
        if (app.market_storage_sampled_at == 0 ||
            storage_now - app.market_storage_sampled_at >= 10000) {
            app.market_storage = database.LoadMarketDataStorageUsage();
            app.market_storage_sampled_at = storage_now;
        }
        if (app.writer_telemetry_sampled_at == 0 ||
            storage_now -
                    app.writer_telemetry_sampled_at >=
                500) {
            app.writer_telemetry =
                database.WriterTelemetry();
            app.writer_telemetry_sampled_at = storage_now;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        if (app.ui_scale.HandleShortcuts()) {
            app.workspace.SetUiScale(app.ui_scale.Scale());
            database.SaveAppSetting("ui.scale",
                                    std::to_string(app.ui_scale.Scale()));
        }

        const float menu_bar_height = DrawMainMenuBar(app);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        // BeginMainMenuBar reports its work inset for the following frame.
        // Use its measured height now so first-use window geometry cannot be
        // persisted underneath the menu bar on the first rendered frame.
        const ImVec2 workspace_pos(viewport->Pos.x,
                                   viewport->Pos.y + menu_bar_height);
        const ImVec2 workspace_size(
            viewport->Size.x,
            std::max(0.0f, viewport->Size.y - menu_bar_height));
        app.workspace.BeginFrame(workspace_pos, workspace_size, true);
        DrawAccount(app);
        DrawWatchlist(app);
        DrawActivity(app);
        DrawCredentials(app, window);
        DrawOrderEntry(app);
        DrawQuickOrder(app);
        DrawOcoOrder(app);
        DrawOrderManagement(app);
        DrawTimeSales(app);
        DrawCharts(app);
        DrawPerformanceOverlay(app);
        app.workspace.EndFrame();

        ImGui::Render();
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        if (!ResizeDx11Renderer(renderer, width, height, renderer_error)) {
            AddMessage(app, renderer_error);
            done = true;
            continue;
        }
        const float clear_color[4] = {0.035f, 0.043f, 0.060f, 1.0f};
        renderer.context->OMSetRenderTargets(
            1, renderer.render_target.GetAddressOf(), nullptr);
        renderer.context->ClearRenderTargetView(renderer.render_target.Get(),
                                                clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (app.capture_requested) {
            const bool waiting_for_connected_smoke_state =
                app.exit_after_capture &&
                (SDL_GetTicks() - launched_at < 6000 ||
                 (SDL_GetTicks() - launched_at < 10000 &&
                  (!app.has_account || app.positions_received_at_ms == 0 ||
                   app.orders_received_at_ms == 0)));
            if (waiting_for_connected_smoke_state) {
                // Let asynchronous account state reach the UI before a
                // command-line framebuffer smoke capture.
            } else if (app.frames_before_capture > 0) {
                --app.frames_before_capture;
            } else {
                std::string error;
                if (CaptureDx11Framebuffer(renderer, width, height,
                                            app.capture_path,
                                             error)) {
                    AddMessage(app, "Framebuffer saved: " +
                                        app.capture_path.string());
                    const nlohmann::json payload = {
                        {"path", app.capture_path.string()},
                        {"width", width},
                        {"height", height},
                    };
                    database.QueueTimelineEvent(
                        "tradebox.ui", "", "framebuffer_capture", "",
                        SystemNowMs(), payload.dump());
                } else {
                    AddMessage(app, "Framebuffer capture failed: " + error);
                }
                app.capture_requested = false;
                if (app.exit_after_capture) done = true;
            }
        }
        renderer.swap_chain->Present(app.vsync_requested ? 1 : 0, 0);
        if (!window_shown) {
            SDL_ShowWindow(window);
            window_shown = true;
        }
        if (app.presentation_rearm_frames > 0 &&
            --app.presentation_rearm_frames == 0) {
            ApplyPresentationSettings(app, window, false);
        }
        PaceSoftwareFrame(app);
    }

    database.SaveWindowPlacement(
        {normal_x, normal_y, normal_width, normal_height, remember_maximized,
         true});
    SaveWindowVisibility(app);
    const auto disconnect_result = app.application.Disconnect();
    static_cast<void>(disconnect_result);
    if (io.IniFilename != nullptr)
        ImGui::SaveIniSettingsToDisk(io.IniFilename);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    renderer.render_target.Reset();
    renderer.swap_chain.Reset();
    renderer.context.Reset();
    renderer.device.Reset();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

}  // namespace

LaunchOptions ParseLaunchOptions(int argc, char* argv[]) {
    LaunchOptions options;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--capture-framebuffer" &&
            index + 1 < argc) {
            options.capture_and_exit = true;
            const char* utf8_path = argv[++index];
            options.capture_path = std::filesystem::path(
                std::u8string(
                    reinterpret_cast<const char8_t*>(utf8_path)));
        } else if (std::string_view(argv[index]) == "--connect-paper") {
            options.connect_paper = true;
        } else if (std::string_view(argv[index]) == "--run-for-ms" &&
                   index + 1 < argc) {
            options.run_for_ms = std::max(0, std::atoi(argv[++index]));
        }
    }
    return options;
}

int main(int argc, char* argv[]) {
    return RunApplication(ParseLaunchOptions(argc, argv));
}
