#include "tradebox/application/trading_application.h"
#include "tradebox/persistence/database.h"
#include "tradebox/ui/model.h"
#include "tradebox/ui/workspace.h"
#include "tradebox/workstation/chart_documents.h"
#include "tradebox/workstation/profile_store.h"
#include "tradebox/workstation/watch_list_documents.h"

#include "chart_window.h"
#include "debug_window.h"
#include "order_ticket_window.h"
#include "positions_window.h"
#include "watch_list_window.h"
#include "trade_hotkey_window.h"
#include "tradebox/workstation/positions_orders_windows.h"
#include "account_popup.h"
#include "application_chrome.h"
#include "gui_support.h"
#include "native_chrome_layout.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_sdl3.h"

namespace {

using Microsoft::WRL::ComPtr;
using tradebox::gui::AccountPopupAction;
using tradebox::gui::AccountPopupState;
using tradebox::gui::ChromeActions;
using tradebox::gui::ChromeMetrics;
using tradebox::gui::DrawApplicationChrome;
using tradebox::gui::DrawImGuiDemo;
using tradebox::gui::DebugSnapshot;
using tradebox::gui::DebugWindowRenderer;
using tradebox::gui::GuiFonts;
using tradebox::gui::ConfigureImGuiStyle;

constexpr float kRegularFontSize = 18.0f;
constexpr float kTitleFontSize = 20.0f;
constexpr float kMenuFontSize = 18.0f;
constexpr float kToolIconSize = 24.0f;

struct Dx11Renderer {
    ~Dx11Renderer() {
        if (frame_latency_waitable != nullptr)
            CloseHandle(frame_latency_waitable);
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11RenderTargetView> render_target;
    HANDLE frame_latency_waitable = nullptr;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    std::string adapter_name;
    std::uint32_t adapter_vendor_id = 0;
    std::uint32_t adapter_device_id = 0;
    std::uint64_t dedicated_video_memory = 0;
    std::uint64_t shared_system_memory = 0;
    bool vsync_requested = true;
    int width = 0;
    int height = 0;
};

struct ResizePreviewContext {
    Dx11Renderer* renderer = nullptr;
    HWND window = nullptr;
};

struct LaunchOptions {
    int run_for_ms = 0;
    std::filesystem::path workspace_path;
    bool read_only_workspace = false;
};

struct DatabaseStartupResult {
    std::unique_ptr<Database> database;
    std::string error;
};

struct ApplicationStartupResult {
    // Destruction order matters: the application borrows the database.
    std::unique_ptr<tradebox::application::TradingApplication> application;
    std::unique_ptr<Database> database;
    std::string error;
};

struct MarketTimeZone {
    DYNAMIC_TIME_ZONE_INFORMATION value{};
    bool available = false;
};

std::string WideToUtf8(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') return {};
    const int length = static_cast<int>(std::wcslen(value));
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8, 0, value, length, result.data(), size, nullptr, nullptr));
    return result;
}

std::string FeatureLevelName(D3D_FEATURE_LEVEL level) {
    switch (level) {
    case D3D_FEATURE_LEVEL_9_1: return "9.1";
    case D3D_FEATURE_LEVEL_9_2: return "9.2";
    case D3D_FEATURE_LEVEL_9_3: return "9.3";
    case D3D_FEATURE_LEVEL_10_0: return "10.0";
    case D3D_FEATURE_LEVEL_10_1: return "10.1";
    case D3D_FEATURE_LEVEL_11_0: return "11.0";
    case D3D_FEATURE_LEVEL_11_1: return "11.1";
    case D3D_FEATURE_LEVEL_12_0: return "12.0";
    case D3D_FEATURE_LEVEL_12_1: return "12.1";
    case D3D_FEATURE_LEVEL_12_2: return "12.2";
    default: return "Unknown";
    }
}


DatabaseStartupResult OpenDatabaseInBackground() {
    auto database = std::make_unique<Database>();
    std::string error;
    if (!database->Open(error))
        return {nullptr, std::move(error)};
    return {std::move(database), {}};
}

ApplicationStartupResult OpenApplicationInBackground(
    std::unique_ptr<Database> database, UiEventQueue& events) {
    try {
        auto application =
            std::make_unique<tradebox::application::TradingApplication>(
                events, *database);
        return {std::move(application), std::move(database), {}};
    } catch (const std::exception& error) {
        return {nullptr, std::move(database), error.what()};
    }
}

MarketTimeZone LoadMarketTimeZone() {
    MarketTimeZone result;
    for (DWORD index = 0;; ++index) {
        DYNAMIC_TIME_ZONE_INFORMATION candidate{};
        const DWORD status =
            EnumDynamicTimeZoneInformation(index, &candidate);
        if (status != ERROR_SUCCESS) break;
        if (std::wstring_view(candidate.TimeZoneKeyName) ==
            L"Eastern Standard Time") {
            result.value = candidate;
            result.available = true;
            break;
        }
    }
    return result;
}

std::string MarketTimeText(const MarketTimeZone& time_zone) {
    if (!time_zone.available) return "--:-- ET";
    SYSTEMTIME utc{};
    SYSTEMTIME eastern{};
    GetSystemTime(&utc);
    if (!SystemTimeToTzSpecificLocalTimeEx(
            &time_zone.value, &utc, &eastern))
        return "--:-- ET";
    const std::array<char, 6> time =
        tradebox::ui::win32::FormatHourMinute(
            eastern.wHour, eastern.wMinute);
    return std::string(time.data()) + " ET";
}

bool CreateRenderTarget(Dx11Renderer& renderer) {
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(renderer.swap_chain->GetBuffer(
            0, IID_PPV_ARGS(back_buffer.GetAddressOf()))))
        return false;
    return SUCCEEDED(renderer.device->CreateRenderTargetView(
        back_buffer.Get(), nullptr, renderer.render_target.GetAddressOf()));
}

HWND NativeWindowHandle(SDL_Window* window) {
    return reinterpret_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr));
}

bool ResizeRenderer(Dx11Renderer& renderer, int width, int height) {
    // SDL can report a zero-sized drawable while a borderless window is being
    // minimized or resized. There is no valid swap-chain surface to render to
    // in that state; the frame loop handles it by skipping presentation.
    if (width <= 0 || height <= 0) return false;
    if (renderer.render_target && renderer.width == width &&
        renderer.height == height)
        return true;

    // ResizeBuffers requires every reference to the current back buffers to
    // be released. ImGui restores most of the pipeline state after drawing,
    // but clearing the context here also releases any retained views before
    // DXGI is asked to replace the buffers.
    renderer.context->OMSetRenderTargets(0, nullptr, nullptr);
    renderer.context->ClearState();
    renderer.context->Flush();
    renderer.render_target.Reset();
    if (FAILED(renderer.swap_chain->ResizeBuffers(
            0, static_cast<UINT>(width), static_cast<UINT>(height),
            DXGI_FORMAT_UNKNOWN,
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)))
        return false;
    if (!CreateRenderTarget(renderer)) {
        renderer.width = 0;
        renderer.height = 0;
        return false;
    }
    renderer.width = width;
    renderer.height = height;
    return true;
}

void PresentResizeShield(Dx11Renderer& renderer) {
    if (!renderer.context || !renderer.swap_chain || !renderer.render_target)
        return;

    constexpr float kResizeShieldColor[4] = {0.16f, 0.17f, 0.19f, 1.0f};
    renderer.context->OMSetRenderTargets(
        1, renderer.render_target.GetAddressOf(), nullptr);
    renderer.context->ClearRenderTargetView(
        renderer.render_target.Get(), kResizeShieldColor);
    static_cast<void>(renderer.swap_chain->Present(0, 0));
}

bool SDLCALL WindowsMessageHook(void* data, MSG* message) {
    auto* context = static_cast<ResizePreviewContext*>(data);
    if (context != nullptr && context->renderer != nullptr &&
        message != nullptr && message->hwnd == context->window &&
        message->message == WM_ENTERSIZEMOVE) {
        // Windows begins its modal resize loop immediately after this message.
        // Replace the last UI frame once so the compositor stretches a neutral
        // surface rather than stale workspace content during the drag.
        PresentResizeShield(*context->renderer);
    }
    return true;
}

bool CreateRenderer(SDL_Window* window, Dx11Renderer& renderer) {
    const HWND hwnd = NativeWindowHandle(window);
    if (!hwnd) return false;

    D3D_FEATURE_LEVEL feature_level{};
    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, renderer.device.GetAddressOf(),
            &feature_level, renderer.context.GetAddressOf())))
        return false;

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory> factory;
    if (FAILED(renderer.device.As(&dxgi_device)) ||
        FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf())) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
        return false;
    renderer.feature_level = feature_level;
    DXGI_ADAPTER_DESC adapter_description{};
    if (SUCCEEDED(adapter->GetDesc(&adapter_description))) {
        renderer.adapter_name = WideToUtf8(adapter_description.Description);
        renderer.adapter_vendor_id = adapter_description.VendorId;
        renderer.adapter_device_id = adapter_description.DeviceId;
        renderer.dedicated_video_memory =
            adapter_description.DedicatedVideoMemory;
        renderer.shared_system_memory =
            adapter_description.SharedSystemMemory;
    }

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    description.OutputWindow = hwnd;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChain(
            renderer.device.Get(), &description,
            renderer.swap_chain.GetAddressOf())))
        return false;

    ComPtr<IDXGISwapChain2> low_latency_swap_chain;
    if (FAILED(renderer.swap_chain.As(&low_latency_swap_chain)) ||
        FAILED(low_latency_swap_chain->SetMaximumFrameLatency(1)))
        return false;
    renderer.frame_latency_waitable =
        low_latency_swap_chain->GetFrameLatencyWaitableObject();
    if (renderer.frame_latency_waitable == nullptr) return false;

    SDL_GetWindowSizeInPixels(window, &renderer.width, &renderer.height);
    return CreateRenderTarget(renderer);
}

DebugSnapshot BuildDebugSnapshot(
    const Dx11Renderer& renderer, SDL_Window* window,
    const tradebox::workstation::ApplicationSettings& settings,
    double frames_per_second, double frame_time_ms) {
    DebugSnapshot snapshot;
    snapshot.frames_per_second = frames_per_second;
    snapshot.frame_time_ms = frame_time_ms;
    snapshot.vsync_requested = settings.vsync_requested;
    snapshot.frame_latency_waitable =
        renderer.frame_latency_waitable != nullptr;
    snapshot.maximum_frame_rate = settings.maximum_frame_rate;
    SDL_GetWindowSizeInPixels(
        window, &snapshot.window_width, &snapshot.window_height);
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display != 0) {
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
            mode != nullptr) {
            snapshot.display_width = mode->w;
            snapshot.display_height = mode->h;
            snapshot.display_refresh_rate = mode->refresh_rate;
        }
    }
    snapshot.adapter_name = renderer.adapter_name;
    snapshot.adapter_vendor_id = renderer.adapter_vendor_id;
    snapshot.adapter_device_id = renderer.adapter_device_id;
    snapshot.dedicated_video_memory = renderer.dedicated_video_memory;
    snapshot.shared_system_memory = renderer.shared_system_memory;
    snapshot.feature_level = FeatureLevelName(renderer.feature_level);
    const int sdl_version = SDL_GetVersion();
    snapshot.sdl_version =
        std::to_string(SDL_VERSIONNUM_MAJOR(sdl_version)) + "." +
        std::to_string(SDL_VERSIONNUM_MINOR(sdl_version)) + "." +
        std::to_string(SDL_VERSIONNUM_MICRO(sdl_version));
    snapshot.platform = SDL_GetPlatform();
    snapshot.logical_cpu_cores = SDL_GetNumLogicalCPUCores();
    snapshot.system_memory_mb = SDL_GetSystemRAM();
#if defined(_MSC_VER)
    snapshot.compiler = "MSVC " + std::to_string(_MSC_VER);
#else
    snapshot.compiler = "Unknown compiler";
#endif
#if defined(_MSVC_STL_VERSION)
    snapshot.stl = "MSVC STL " + std::to_string(_MSVC_STL_VERSION);
#else
    snapshot.stl = "Unknown STL";
#endif
#if defined(_MSVC_LANG)
    snapshot.cxx_standard =
        "__cplusplus=" + std::to_string(__cplusplus) +
        ", _MSVC_LANG=" + std::to_string(_MSVC_LANG);
#else
    snapshot.cxx_standard = "__cplusplus=" + std::to_string(__cplusplus);
#endif
    snapshot.steady_clock =
        std::chrono::steady_clock::is_steady ? "steady" : "not steady";
    return snapshot;
}

std::filesystem::path AssetPath(std::string_view relative_path) {
    const char* base_path = SDL_GetBasePath();
    const std::filesystem::path result =
        (base_path != nullptr ? std::filesystem::path(base_path)
                              : std::filesystem::current_path()) /
        "assets" / relative_path;
    return result;
}

bool LoadGuiFonts(GuiFonts& fonts) {
    ImGuiIO& io = ImGui::GetIO();
    fonts.regular = io.Fonts->AddFontFromFileTTF(
        AssetPath("fonts/B612-Regular.ttf").string().c_str(),
        kRegularFontSize,
        nullptr, io.Fonts->GetGlyphRangesDefault());
    fonts.mono = io.Fonts->AddFontFromFileTTF(
        AssetPath("fonts/B612Mono-Regular.ttf").string().c_str(),
        kRegularFontSize,
        nullptr, io.Fonts->GetGlyphRangesDefault());
    fonts.title = io.Fonts->AddFontFromFileTTF(
        AssetPath("fonts/B612-Regular.ttf").string().c_str(),
        kTitleFontSize, nullptr, io.Fonts->GetGlyphRangesDefault());
    fonts.icons = io.Fonts->AddFontFromFileTTF(
        AssetPath("fonts/MaterialSymbolsRounded.ttf").string().c_str(),
        kToolIconSize, nullptr, fonts.icon_ranges.data());
    if (fonts.regular == nullptr || fonts.mono == nullptr ||
        fonts.title == nullptr ||
        fonts.icons == nullptr)
        return false;
    io.FontDefault = fonts.regular;
    return true;
}

SDL_HitTestResult SDLCALL HitTestChrome(
    SDL_Window* window, const SDL_Point* area, void* data) {
    if (area == nullptr || data == nullptr) return SDL_HITTEST_NORMAL;
    const auto& metrics = *static_cast<const ChromeMetrics*>(data);
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    const bool maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;

    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    constexpr int kResizeBorder = 8;
    if (!maximized) {
        if (area->x < kResizeBorder) {
            if (area->y < kResizeBorder) return SDL_HITTEST_RESIZE_TOPLEFT;
            if (area->y >= height - kResizeBorder)
                return SDL_HITTEST_RESIZE_BOTTOMLEFT;
            return SDL_HITTEST_RESIZE_LEFT;
        }
        if (area->x >= width - kResizeBorder) {
            if (area->y < kResizeBorder) return SDL_HITTEST_RESIZE_TOPRIGHT;
            if (area->y >= height - kResizeBorder)
                return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
            return SDL_HITTEST_RESIZE_RIGHT;
        }
        if (area->y >= height - kResizeBorder)
            return SDL_HITTEST_RESIZE_BOTTOM;
        if (area->y < kResizeBorder) return SDL_HITTEST_RESIZE_TOP;
    }

    if (tradebox::ui::win32::IsTitleBarDragPoint(
            area->x, area->y, metrics.title_bar_height,
            metrics.interactive_left_width, width,
            metrics.control_width * 3))
        return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

LaunchOptions ParseLaunchOptions(int argc, char* argv[]) {
    LaunchOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--run-for-ms" && index + 1 < argc) {
            options.run_for_ms = std::max(0, std::atoi(argv[++index]));
        } else if (argument == "--workspace" && index + 1 < argc) {
            options.workspace_path = std::filesystem::path(argv[++index]);
        } else if (argument == "--read-only") {
            options.read_only_workspace = true;
        }
    }
    return options;
}

bool CaptureNativeWindowState(
    SDL_Window* window,
    tradebox::workstation::NativeWindowState& state) {
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    const bool maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    bool changed = state.maximized != maximized;
    state.maximized = maximized;
    if (state.maximized || (flags & SDL_WINDOW_MINIMIZED) != 0)
        return changed;

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (SDL_GetWindowPosition(window, &x, &y) &&
        SDL_GetWindowSize(window, &width, &height)) {
        const tradebox::workstation::LogicalRect next_bounds{
            static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(width), static_cast<float>(height)};
        changed = changed ||
                  std::fabs(state.bounds.x - next_bounds.x) > 0.25f ||
                  std::fabs(state.bounds.y - next_bounds.y) > 0.25f ||
                  std::fabs(state.bounds.width - next_bounds.width) > 0.25f ||
                  std::fabs(state.bounds.height - next_bounds.height) > 0.25f;
        state.bounds = next_bounds;
    }
    return changed;
}

std::vector<std::string> VisibleMarketSymbols(
    const tradebox::application::UiSnapshotQuery& query,
    const tradebox::core::CoreSnapshot& core) {
    std::vector<std::string> result = query.market_symbols;
    for (const auto& chart : query.charts)
        if (!chart.symbol.empty()) result.push_back(chart.symbol);
    if (query.include_position_markets)
        for (const auto& position : core.positions)
            result.push_back(position.symbol);
    std::ranges::sort(result);
    const auto unique = std::ranges::unique(result);
    result.erase(unique.begin(), unique.end());
    std::erase_if(result,
                  [](const std::string& symbol) { return symbol.empty(); });
    return result;
}

void DrawStartupOverlay(std::string_view status) {
    if (status.empty()) return;
    const std::string text(status);
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.025f, 0.031f, 0.045f, 0.82f});
    if (ImGui::Begin("##startup_overlay", nullptr, flags)) {
        const ImVec2 size = ImGui::GetWindowSize();
        const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
        ImGui::SetCursorPos({
            std::max(24.0f, (size.x - text_size.x) * 0.5f),
            std::max(24.0f, (size.y - text_size.y) * 0.5f)});
        ImGui::TextUnformatted(text.c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void ClearCredentialInputs(AccountPopupState& state) {
    SecureZeroMemory(state.api_key.data(), state.api_key.size());
    state.api_key.fill('\0');
    SecureZeroMemory(state.api_secret.data(), state.api_secret.size());
    state.api_secret.fill('\0');
}

std::string CredentialSlot(
    const tradebox::workstation::AccountContext& account_context,
    tradebox::core::AccountEnvironment environment) {
    if (!account_context.credential_slot.empty())
        return account_context.credential_slot;
    return environment == tradebox::core::AccountEnvironment::Paper
               ? "alpaca-paper-default"
               : "alpaca-live-default";
}

int RunApplication(const LaunchOptions& options) {
    tradebox::workstation::ProfileStore profile_store;
    const std::filesystem::path profile_path =
        options.workspace_path.empty()
            ? tradebox::workstation::ProfileStore::DefaultProfilePath()
            : options.workspace_path;
    const bool profile_existed = std::filesystem::exists(profile_path);
    std::string profile_error;
    if (!profile_store.Open(profile_path, options.read_only_workspace,
                            profile_error)) {
        MessageBoxA(nullptr, profile_error.c_str(),
                    "Trade Box profile error", MB_OK | MB_ICONERROR);
        return 1;
    }
    const auto loaded_profile = profile_store.Load(profile_path);
    if (!loaded_profile) {
        MessageBoxA(nullptr, loaded_profile.error().c_str(),
                    "Trade Box profile error", MB_OK | MB_ICONERROR);
        return 1;
    }
    tradebox::workstation::WorkstationState workstation_state =
        *loaded_profile;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return 1;
    const auto& native_window = workstation_state.native_window;
    const int initial_width = std::max(
        640, static_cast<int>(native_window.bounds.width));
    const int initial_height = std::max(
        480, static_cast<int>(native_window.bounds.height));
    SDL_Window* window = SDL_CreateWindow(
        "Trade Box", initial_width, initial_height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS |
            SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowPosition(window,
                          static_cast<int>(native_window.bounds.x),
                          static_cast<int>(native_window.bounds.y));
    if (native_window.maximized) SDL_MaximizeWindow(window);
    ChromeMetrics chrome_metrics;
    if (!SDL_SetWindowHitTest(window, HitTestChrome, &chrome_metrics)) {
        MessageBoxA(nullptr, SDL_GetError(), "Trade Box window error",
                    MB_OK | MB_ICONERROR);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    Dx11Renderer renderer;
    if (!CreateRenderer(window, renderer)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureImGuiStyle();
    ImGui::GetIO().IniFilename = nullptr;
    GuiFonts gui_fonts;
    if (!LoadGuiFonts(gui_fonts)) {
        MessageBoxA(nullptr,
                    "Trade Box could not load its bundled UI assets.",
                    "Trade Box asset error", MB_OK | MB_ICONERROR);
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    ImGui_ImplSDL3_InitForD3D(window);
    ImGui_ImplDX11_Init(renderer.device.Get(), renderer.context.Get());
    ResizePreviewContext resize_preview{&renderer, NativeWindowHandle(window)};
    SDL_SetWindowsMessageHook(WindowsMessageHook, &resize_preview);

    // The window is usable before local database startup completes. In
    // particular, opening a large market-data store or running a one-time
    // migration must not block the first frame.
    SDL_SetWindowTitle(window, "Trade Box - Loading local data...");
    UiEventQueue events;
    std::future<DatabaseStartupResult> database_startup = std::async(
        std::launch::async, OpenDatabaseInBackground);
    std::future<ApplicationStartupResult> application_startup;
    std::unique_ptr<Database> database;
    std::unique_ptr<tradebox::application::TradingApplication> application;
    std::string startup_status = "Opening local market database...";
    tradebox::ui::Workspace workspace;
    workspace.SetPersistentState(&workstation_state.workspace);
    tradebox::gui::ChartWindowRenderer chart_renderer;
    DebugWindowRenderer debug_renderer;
    tradebox::gui::WatchListWindowRenderer watch_list_renderer;
    tradebox::gui::TradeHotkeyWindowRenderer trade_hotkey_renderer;
    tradebox::gui::OrderTicketWindowRenderer order_ticket_renderer;
    tradebox::gui::PositionsWindowRenderer positions_renderer;
    tradebox::gui::OrdersWindowRenderer orders_renderer;

    if (!profile_existed) profile_store.MarkDirty();

    bool done = false;
    AccountPopupState account_popup;
    const std::string initial_account_name =
        workstation_state.account_context.account_alias;
    std::copy_n(
        initial_account_name.c_str(),
        std::min(initial_account_name.size(),
                 account_popup.account_name.size() - 1U),
        account_popup.account_name.data());
    account_popup.environment =
        workstation_state.account_context.paper
            ? tradebox::core::AccountEnvironment::Paper
            : tradebox::core::AccountEnvironment::Live;
    bool show_imgui_demo = false;
    const MarketTimeZone market_time_zone = LoadMarketTimeZone();
    const Uint64 started_at = SDL_GetTicks();
    auto previous_frame_start = std::chrono::steady_clock::now();
    double measured_frames_per_second = 0.0;
    double measured_frame_time_ms = 0.0;
    while (!done) {
        const auto frame_start = std::chrono::steady_clock::now();
        measured_frame_time_ms = std::chrono::duration<double, std::milli>(
                                     frame_start - previous_frame_start)
                                     .count();
        if (measured_frame_time_ms > 0.0) {
            const double instantaneous_fps = 1000.0 / measured_frame_time_ms;
            measured_frames_per_second =
                measured_frames_per_second == 0.0
                    ? instantaneous_fps
                    : measured_frames_per_second * 0.9 +
                          instantaneous_fps * 0.1;
        }
        previous_frame_start = frame_start;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                done = true;
        }
        if (options.run_for_ms > 0 &&
            SDL_GetTicks() - started_at >=
            static_cast<Uint64>(options.run_for_ms))
            done = true;

        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0) {
            SDL_Delay(10);
            continue;
        }

        if (renderer.frame_latency_waitable != nullptr)
            static_cast<void>(WaitForSingleObject(
                renderer.frame_latency_waitable, 1000));

        if (!application && !application_startup.valid() &&
            database_startup.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
            DatabaseStartupResult result = database_startup.get();
            if (!result.error.empty()) {
                startup_status = "Database startup failed: " + result.error;
            } else {
                startup_status =
                    "Starting trading services and loading cached data...";
                application_startup = std::async(
                    std::launch::async,
                    [&events](std::unique_ptr<Database> ready_database) {
                        return OpenApplicationInBackground(
                            std::move(ready_database), events);
                    },
                    std::move(result.database));
            }
        }

        if (!application && application_startup.valid() &&
            application_startup.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
            ApplicationStartupResult result = application_startup.get();
            if (!result.error.empty()) {
                startup_status = "Trading services startup failed: " +
                                 result.error;
            } else {
                database = std::move(result.database);
                application = std::move(result.application);
                startup_status.clear();
                SDL_SetWindowTitle(window, "Trade Box");
                if (workstation_state.account_context.auto_connect) {
                    const tradebox::core::AccountEnvironment environment =
                        workstation_state.account_context.paper
                            ? tradebox::core::AccountEnvironment::Paper
                            : tradebox::core::AccountEnvironment::Live;
                    const std::string slot = CredentialSlot(
                        workstation_state.account_context, environment);
                    if (application->HasSavedCredentials(slot, environment)) {
                        tradebox::application::ConnectionRequest request;
                        request.environment = environment;
                        request.credential_slot = slot;
                        request.market_data_feed =
                            tradebox::core::MarketDataFeed::Iex;
                        const auto receipt =
                            application->Connect(std::move(request));
                        account_popup.message = receipt
                                                     ? receipt->message
                                                     : receipt.error().message;
                    } else {
                        account_popup.message =
                            "Auto-connect is enabled, but no saved credentials "
                            "were found for the last account";
                    }
                }
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        const std::int64_t now_ns = now.time_since_epoch().count();
        if (application != nullptr)
            static_cast<void>(application->DrainUiEvents());
        auto snapshot_query = chart_renderer.BuildSnapshotQuery(
            workstation_state.workspace, now_ns);
        snapshot_query.as_of_ns = now_ns;
        watch_list_renderer.AppendSnapshotQuery(
            workstation_state.workspace, snapshot_query);
        positions_renderer.AppendSnapshotQuery(
            workstation_state.workspace, snapshot_query);
        if (application != nullptr) {
            const auto core_snapshot = application->Snapshot();
            static_cast<void>(application->UpdateMarketDataInterest({
                .consumer_id = "ui.visible",
                .feed = tradebox::core::MarketDataFeed::Iex,
                .symbols = VisibleMarketSymbols(snapshot_query, core_snapshot),
                .priority = tradebox::application::
                    MarketDataInterestPriority::UserVisible,
            }));
        }
        const tradebox::application::ApplicationUiSnapshot snapshot =
            application != nullptr
                ? application->SnapshotForUi(snapshot_query)
                : tradebox::application::ApplicationUiSnapshot{};
        std::vector<tradebox::application::SavedAccountDescriptor>
            saved_accounts;
        std::string saved_accounts_error;
        if (application != nullptr) {
            const auto listed_accounts = application->SavedAccounts();
            if (listed_accounts)
                saved_accounts = std::move(*listed_accounts);
            else
                saved_accounts_error = listed_accounts.error();
        }
        if (snapshot.core.authenticated && snapshot.core.account &&
            workstation_state.account_context.account_id !=
                snapshot.core.account->id) {
            workstation_state.account_context.account_id =
                snapshot.core.account->id;
            profile_store.MarkDirty();
        }
        const std::string credential_slot =
            CredentialSlot(workstation_state.account_context,
                           account_popup.environment);
        const tradebox::core::AccountEnvironment current_environment =
            workstation_state.account_context.paper
                ? tradebox::core::AccountEnvironment::Paper
                : tradebox::core::AccountEnvironment::Live;

        const ChromeActions chrome_actions = DrawApplicationChrome(
            window, chrome_metrics, gui_fonts,
            MarketTimeText(market_time_zone), snapshot.core,
            workstation_state.account_context.account_alias,
            application != nullptr, account_popup, saved_accounts,
            saved_accounts_error, credential_slot,
            current_environment,
            workstation_state.account_context.account_id,
            workstation_state.account_context.auto_connect,
            workstation_state.workspace,
            workstation_state.application, done);
        if (chrome_actions.new_chart) {
            const auto created = tradebox::workstation::CreateChartDocument(
                workstation_state.workspace);
            if (!created) {
                MessageBoxA(nullptr, created.error().message.c_str(),
                            "Trade Box chart error", MB_OK | MB_ICONERROR);
                done = true;
            } else {
                profile_store.MarkDirty();
            }
        }
        if (chrome_actions.new_watch_list) {
            watch_list_renderer.StartNewDraft(workstation_state.workspace);
        }
        if (chrome_actions.new_positions) {
            const auto created = tradebox::workstation::CreatePositionsWindow(
                workstation_state.workspace);
            if (created) profile_store.MarkDirty();
        }
        if (chrome_actions.new_orders) {
            const auto created = tradebox::workstation::CreateOrdersWindow(
                workstation_state.workspace);
            if (created) profile_store.MarkDirty();
        }
        if (chrome_actions.open_watch_list_id) {
            watch_list_renderer.OpenSavedDocument(
                workstation_state.workspace, *chrome_actions.open_watch_list_id);
        }
        if (chrome_actions.new_debug) debug_renderer.Open();
        if (chrome_actions.imgui_demo) show_imgui_demo = true;
        if (chrome_actions.settings_changed) profile_store.MarkDirty();

        if (chrome_actions.account == AccountPopupAction::SaveAccount) {
            const std::string account_name = account_popup.account_name.data();
            const tradebox::core::AccountEnvironment environment =
                account_popup.environment;
            if (application == nullptr) {
                account_popup.message = "Local application is still loading";
            } else if (application->HasSavedCredentials(
                           account_name, environment)) {
                account_popup.message =
                    "An account with that name already exists";
                ClearCredentialInputs(account_popup);
            } else {
                const auto saved = application->SaveCredentials(
                    account_name, environment, account_popup.api_key.data(),
                    account_popup.api_secret.data());
                if (!saved) {
                    account_popup.message = saved.error();
                    ClearCredentialInputs(account_popup);
                } else {
                    account_popup.message = "Account saved";
                    account_popup.adding_account = false;
                    ClearCredentialInputs(account_popup);
                }
            }
        } else if (chrome_actions.account == AccountPopupAction::EditName) {
            const std::string old_slot = CredentialSlot(
                workstation_state.account_context,
                workstation_state.account_context.paper
                    ? tradebox::core::AccountEnvironment::Paper
                    : tradebox::core::AccountEnvironment::Live);
            const bool account_changed =
                old_slot != account_popup.selected_credential_slot ||
                workstation_state.account_context.paper !=
                    (account_popup.selected_environment ==
                     tradebox::core::AccountEnvironment::Paper);
            workstation_state.account_context.paper =
                account_popup.selected_environment ==
                tradebox::core::AccountEnvironment::Paper;
            workstation_state.account_context.credential_slot =
                account_popup.selected_credential_slot;
            workstation_state.account_context.account_alias =
                account_popup.account_name.data();
            if (account_changed)
                workstation_state.account_context.account_id.clear();
            profile_store.MarkDirty();
        } else if (chrome_actions.account == AccountPopupAction::SaveName) {
            const std::string old_slot =
                account_popup.selected_credential_slot;
            const std::string new_slot = account_popup.account_name.data();
            const tradebox::core::AccountEnvironment environment =
                account_popup.selected_environment;
            if (application == nullptr) {
                account_popup.message = "Local application is still loading";
            } else {
                const auto renamed = application->RenameAccount(
                    old_slot, new_slot, environment);
                if (!renamed) {
                    account_popup.message = renamed.error();
                } else {
                    workstation_state.account_context.paper =
                        environment == tradebox::core::AccountEnvironment::Paper;
                    workstation_state.account_context.credential_slot = new_slot;
                    workstation_state.account_context.account_alias = new_slot;
                    account_popup.selected_credential_slot = new_slot;
                    account_popup.editing_name = false;
                    account_popup.message.clear();
                    profile_store.MarkDirty();
                }
            }
        } else if (chrome_actions.account == AccountPopupAction::Connect) {
            const bool has_selected_account =
                !account_popup.selected_credential_slot.empty();
            const tradebox::core::AccountEnvironment target_environment =
                has_selected_account ? account_popup.selected_environment
                                     : account_popup.environment;
            const std::string old_credential_slot = CredentialSlot(
                workstation_state.account_context,
                workstation_state.account_context.paper
                    ? tradebox::core::AccountEnvironment::Paper
                    : tradebox::core::AccountEnvironment::Live);
            std::string target_credential_slot =
                account_popup.selected_credential_slot;
            if (account_popup.adding_account)
                target_credential_slot = account_popup.account_name.data();
            if (target_credential_slot.empty())
                target_credential_slot = credential_slot;

            const bool account_changed =
                old_credential_slot != target_credential_slot ||
                workstation_state.account_context.paper !=
                    (target_environment ==
                     tradebox::core::AccountEnvironment::Paper);
            workstation_state.account_context.paper =
                target_environment == tradebox::core::AccountEnvironment::Paper;
            workstation_state.account_context.credential_slot =
                target_credential_slot;
            if (account_popup.account_name[0] != '\0')
                workstation_state.account_context.account_alias =
                    account_popup.account_name.data();
            else if (workstation_state.account_context.account_alias.empty())
                workstation_state.account_context.account_alias =
                    workstation_state.account_context.paper ? "Alpaca Paper"
                                                            : "Alpaca Live";
            if (account_changed)
                workstation_state.account_context.account_id.clear();
            profile_store.MarkDirty();
            if (application == nullptr) {
                account_popup.message = "Local application is still loading";
            } else {
                tradebox::application::ConnectionRequest request;
                request.environment =
                    target_environment;
                request.credential_slot = target_credential_slot;
                request.market_data_feed =
                    tradebox::core::MarketDataFeed::Iex;
                const auto receipt = application->Connect(std::move(request));
                account_popup.message = receipt
                                             ? receipt->message
                                             : receipt.error().message;
            }
            account_popup.live_trading_confirmed = false;
            account_popup.adding_account = false;
            ClearCredentialInputs(account_popup);
        } else if (chrome_actions.account == AccountPopupAction::Disconnect) {
            if (application == nullptr) {
                account_popup.message = "Local application is still loading";
            } else {
                const auto receipt = application->Disconnect();
                account_popup.message = receipt
                                             ? receipt->message
                                             : receipt.error().message;
            }
            account_popup.live_trading_confirmed = false;
        } else if (chrome_actions.account ==
                   AccountPopupAction::ForgetCredentials) {
            if (application == nullptr) {
                account_popup.message = "Local application is still loading";
            } else {
                const std::string target_slot =
                    account_popup.selected_credential_slot.empty()
                        ? credential_slot
                        : account_popup.selected_credential_slot;
                const tradebox::core::AccountEnvironment target_environment =
                    account_popup.selected_credential_slot.empty()
                        ? (snapshot.core.authenticated
                               ? snapshot.core.environment
                               : account_popup.environment)
                        : account_popup.selected_environment;
                const auto forgotten = application->ForgetCredentials(
                    target_slot, target_environment);
                account_popup.message = forgotten ? "" : forgotten.error();
                if (forgotten) {
                    account_popup.remember_credentials = false;
                    account_popup.editing_name = false;
                }
            }
        }
        if (application != nullptr)
            chart_renderer.RequestMissingHistory(*application, snapshot);
        if (application != nullptr)
            watch_list_renderer.RequestMissingHistory(*application, snapshot);

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        workspace.SetUiScale(workstation_state.application.ui_scale);
        workspace.SetSnapPixels(
            workstation_state.application.window_snap_pixels);
        workspace.BeginFrame(
            {viewport->Pos.x,
             viewport->Pos.y + static_cast<float>(chrome_metrics.title_bar_height)},
            {viewport->Size.x,
             std::max(0.0f, viewport->Size.y -
                                static_cast<float>(chrome_metrics.title_bar_height))},
            true);
        chart_renderer.Draw(workspace, workstation_state.workspace, snapshot);
        watch_list_renderer.Draw(
            workspace, workstation_state.workspace, snapshot, gui_fonts.mono,
            gui_fonts.icons);
        order_ticket_renderer.Draw(
            workspace, workstation_state.workspace, snapshot);
        trade_hotkey_renderer.Draw(workspace, workstation_state.workspace);
        positions_renderer.Draw(
            workspace, workstation_state.workspace, snapshot);
        orders_renderer.Draw(
            workspace, workstation_state.workspace, snapshot);
        const DebugSnapshot debug_snapshot = BuildDebugSnapshot(
            renderer, window, workstation_state.application,
            measured_frames_per_second, measured_frame_time_ms);
        debug_renderer.Draw(
            workspace, workstation_state.workspace, debug_snapshot);
        if (application != nullptr) {
            for (const auto& retry : chart_renderer.ConsumeHistoryRetries())
                application->RequestMarketHistory(retry);
        } else {
            static_cast<void>(chart_renderer.ConsumeHistoryRetries());
        }
        workspace.EndFrame();

        DrawStartupOverlay(startup_status);

        DrawImGuiDemo(show_imgui_demo,
                      static_cast<float>(chrome_metrics.title_bar_height));

        if (chart_renderer.ConsumePersistentChanges() ||
            watch_list_renderer.ConsumePersistentChanges() ||
            order_ticket_renderer.ConsumePersistentChanges() ||
            trade_hotkey_renderer.ConsumePersistentChanges() ||
            positions_renderer.ConsumePersistentChanges() ||
            orders_renderer.ConsumePersistentChanges() ||
            workspace.ConsumeDirty())
            profile_store.MarkDirty();

        const bool native_window_changed =
            CaptureNativeWindowState(window, workstation_state.native_window);
        if (native_window_changed) profile_store.MarkDirty();
        std::string save_error;
        if (!profile_store.FlushIfDue(workstation_state, save_error)) {
            MessageBoxA(nullptr, save_error.c_str(),
                        "Trade Box profile error", MB_OK | MB_ICONERROR);
            done = true;
        }
        ImGui::Render();

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        if (width <= 0 || height <= 0) continue;
        if (!ResizeRenderer(renderer, width, height)) {
            // A resize can temporarily fail while Windows is changing the
            // client area. The next frame retries against the current size
            // instead of treating the condition as a close request.
            continue;
        }
        const float clear_color[4] = {0.035f, 0.043f, 0.060f, 1.0f};
        renderer.context->OMSetRenderTargets(
            1, renderer.render_target.GetAddressOf(), nullptr);
        renderer.context->ClearRenderTargetView(
            renderer.render_target.Get(), clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        renderer.vsync_requested =
            workstation_state.application.vsync_requested;
        const HRESULT present_result = renderer.swap_chain->Present(
            renderer.vsync_requested ? 1 : 0, 0);
        if (FAILED(present_result)) {
            // Do not turn a transient presentation failure during a resize
            // into an application-close request; retry on the next frame.
            continue;
        }
    }

    // Ensure background startup futures have completed before their owned
    // objects leave scope. The normal path never waits here because startup
    // is already complete before the application is used.
    if (application_startup.valid()) {
        ApplicationStartupResult result = application_startup.get();
        if (!result.error.empty()) {
            MessageBoxA(nullptr, result.error.c_str(),
                        "Trade Box startup error", MB_OK | MB_ICONERROR);
        } else {
            database = std::move(result.database);
            application = std::move(result.application);
        }
    }
    if (database_startup.valid()) {
        DatabaseStartupResult result = database_startup.get();
        if (!result.error.empty())
            MessageBoxA(nullptr, result.error.c_str(),
                        "Trade Box database error",
                        MB_OK | MB_ICONERROR);
        else
            database = std::move(result.database);
    }

    CaptureNativeWindowState(window, workstation_state.native_window);
    std::string flush_error;
    if (!profile_store.Flush(workstation_state, flush_error)) {
        MessageBoxA(nullptr, flush_error.c_str(),
                    "Trade Box profile error", MB_OK | MB_ICONERROR);
    }

    // Destroy the application and database first so that all background
    // threads (broker service workers, database writer) are joined and all
    // pending writes are flushed before the profile lock is released.
    application.reset();
    database.reset();

    // Release the profile lock before shutting down graphics and window
    // subsystems, so that a crash in those paths does not leave a stale lock
    // file that prevents the next launch.
    profile_store.Close();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_SetWindowsMessageHook(nullptr, nullptr);
    renderer.render_target.Reset();
    renderer.swap_chain.Reset();
    renderer.context.Reset();
    renderer.device.Reset();
    SDL_DestroyWindow(window);
    SDL_Quit();
    ClearCredentialInputs(account_popup);
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    return RunApplication(ParseLaunchOptions(argc, argv));
}
