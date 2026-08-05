#include "tradebox/application/trading_application.h"
#include "tradebox/persistence/database.h"
#include "tradebox/ui/model.h"
#include "tradebox/ui/workspace.h"
#include "tradebox/workstation/chart_documents.h"
#include "tradebox/workstation/profile_store.h"

#include "chart_window.h"
#include "account_popup.h"
#include "application_chrome.h"
#include "gui_support.h"
#include "native_chrome_layout.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
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
using tradebox::gui::GuiFonts;
using tradebox::gui::ConfigureImGuiStyle;

constexpr float kRegularFontSize = 18.0f;
constexpr float kTitleFontSize = 20.0f;
constexpr float kMenuFontSize = 18.0f;
constexpr float kToolIconSize = 24.0f;

struct Dx11Renderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11RenderTargetView> render_target;
    int width = 0;
    int height = 0;
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

struct MarketTimeZone {
    DYNAMIC_TIME_ZONE_INFORMATION value{};
    bool available = false;
};


DatabaseStartupResult OpenDatabaseInBackground() {
    auto database = std::make_unique<Database>();
    std::string error;
    if (!database->Open(error))
        return {nullptr, std::move(error)};
    return {std::move(database), {}};
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
    if (width <= 0 || height <= 0) return true;
    if (renderer.render_target && renderer.width == width &&
        renderer.height == height)
        return true;

    renderer.context->OMSetRenderTargets(0, nullptr, nullptr);
    renderer.render_target.Reset();
    if (FAILED(renderer.swap_chain->ResizeBuffers(
            0, static_cast<UINT>(width), static_cast<UINT>(height),
            DXGI_FORMAT_UNKNOWN, 0)))
        return false;
    renderer.width = width;
    renderer.height = height;
    return CreateRenderTarget(renderer);
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

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.OutputWindow = hwnd;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChain(
            renderer.device.Get(), &description,
            renderer.swap_chain.GetAddressOf())))
        return false;

    SDL_GetWindowSizeInPixels(window, &renderer.width, &renderer.height);
    return CreateRenderTarget(renderer);
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
    fonts.title = io.Fonts->AddFontFromFileTTF(
        AssetPath("fonts/B612-Regular.ttf").string().c_str(),
        kTitleFontSize, nullptr, io.Fonts->GetGlyphRangesDefault());
    fonts.icons = io.Fonts->AddFontFromFileTTF(
        AssetPath("fonts/MaterialSymbolsRounded.ttf").string().c_str(),
        kToolIconSize, nullptr, fonts.icon_ranges.data());
    if (fonts.regular == nullptr || fonts.title == nullptr ||
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

std::vector<std::string> ConnectionSymbols(
    const tradebox::workstation::WorkspaceState& workspace) {
    std::vector<std::string> result;
    const auto add = [&result](const std::string& symbol) {
        if (symbol.empty() ||
            std::ranges::find(result, symbol) != result.end())
            return;
        result.push_back(symbol);
    };
    for (const std::string& symbol : workspace.watchlist) add(symbol);
    for (const auto& chart : workspace.charts) add(chart.symbol);
    return result;
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

    bool chart_created = false;
    if (workstation_state.workspace.charts.empty()) {
        const auto created = tradebox::workstation::CreateChartDocument(
            workstation_state.workspace);
        if (!created) {
            MessageBoxA(nullptr, created.error().message.c_str(),
                        "Trade Box chart error", MB_OK | MB_ICONERROR);
            return 1;
        }
        chart_created = true;
    }

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

    // The window is usable before local database startup completes. In
    // particular, opening a large market-data store or running a one-time
    // migration must not block the first frame.
    SDL_SetWindowTitle(window, "Trade Box - Loading local data...");
    UiEventQueue events;
    std::future<DatabaseStartupResult> database_startup = std::async(
        std::launch::async, OpenDatabaseInBackground);
    std::unique_ptr<Database> database;
    std::unique_ptr<tradebox::application::TradingApplication> application;
    tradebox::ui::Workspace workspace;
    workspace.SetPersistentState(&workstation_state.workspace);
    tradebox::gui::ChartWindowRenderer chart_renderer;

    if (!profile_existed || chart_created) profile_store.MarkDirty();

    bool done = false;
    AccountPopupState account_popup;
    account_popup.environment =
        workstation_state.account_context.paper
            ? tradebox::core::AccountEnvironment::Paper
            : tradebox::core::AccountEnvironment::Live;
    bool show_imgui_demo = false;
    const MarketTimeZone market_time_zone = LoadMarketTimeZone();
    const Uint64 started_at = SDL_GetTicks();
    while (!done) {
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

        if (!application && database_startup.wait_for(
                std::chrono::milliseconds(0)) == std::future_status::ready) {
            DatabaseStartupResult result = database_startup.get();
            if (!result.error.empty()) {
                MessageBoxA(nullptr, result.error.c_str(),
                            "Trade Box database error",
                            MB_OK | MB_ICONERROR);
                done = true;
            } else {
                database = std::move(result.database);
                application = std::make_unique<
                    tradebox::application::TradingApplication>(
                    events, *database);
                SDL_SetWindowTitle(window, "Trade Box");
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        const std::int64_t now_ns = now.time_since_epoch().count();
        const auto snapshot_query = chart_renderer.BuildSnapshotQuery(
            workstation_state.workspace, now_ns);
        const tradebox::application::ApplicationUiSnapshot snapshot =
            application != nullptr
                ? application->SnapshotForUi(snapshot_query)
                : tradebox::application::ApplicationUiSnapshot{};
        const std::string credential_slot =
            CredentialSlot(workstation_state.account_context,
                           account_popup.environment);
        const bool saved_credentials_available =
            application != nullptr && application->HasSavedCredentials(
                credential_slot, account_popup.environment);

        const ChromeActions chrome_actions = DrawApplicationChrome(
            window, chrome_metrics, gui_fonts,
            MarketTimeText(market_time_zone), snapshot.core,
            application != nullptr, account_popup, saved_credentials_available,
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
        if (chrome_actions.imgui_demo) show_imgui_demo = true;
        if (chrome_actions.settings_changed) profile_store.MarkDirty();

        if (chrome_actions.account == AccountPopupAction::Connect) {
            workstation_state.account_context.paper =
                account_popup.environment ==
                tradebox::core::AccountEnvironment::Paper;
            workstation_state.account_context.credential_slot = credential_slot;
            workstation_state.account_context.account_alias =
                workstation_state.account_context.paper ? "Alpaca Paper"
                                                        : "Alpaca Live";
            profile_store.MarkDirty();
            if (application == nullptr) {
                account_popup.message = "Local application is still loading";
            } else {
                bool connect_requested = true;
                tradebox::application::ConnectionRequest request;
                request.environment =
                    account_popup.environment;
                request.credential_slot = credential_slot;
                const bool fields_complete =
                    account_popup.api_key[0] != '\0' &&
                    account_popup.api_secret[0] != '\0';
                if (fields_complete && account_popup.remember_credentials) {
                    const auto saved = application->SaveCredentials(
                        credential_slot, account_popup.environment,
                        account_popup.api_key.data(),
                        account_popup.api_secret.data());
                    if (!saved) {
                        account_popup.message = saved.error();
                        ClearCredentialInputs(account_popup);
                        connect_requested = false;
                    }
                }
                if (connect_requested) {
                    if (fields_complete) {
                        request.api_key = account_popup.api_key.data();
                        request.api_secret = account_popup.api_secret.data();
                    }
                    request.market_symbols =
                        ConnectionSymbols(workstation_state.workspace);
                    request.market_data_feed =
                        tradebox::core::MarketDataFeed::Iex;
                    const auto receipt =
                        application->Connect(std::move(request));
                    account_popup.message = receipt
                                                 ? receipt->message
                                                 : receipt.error().message;
                }
            }
            account_popup.live_trading_confirmed = false;
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
                const auto forgotten = application->ForgetCredentials(
                    credential_slot, snapshot.core.authenticated
                        ? snapshot.core.environment
                        : account_popup.environment);
                account_popup.message = forgotten
                                            ? "Saved credentials forgotten"
                                            : forgotten.error();
                if (forgotten) account_popup.remember_credentials = false;
            }
        }
        if (application != nullptr)
            chart_renderer.RequestMissingHistory(*application, snapshot);

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
        if (application != nullptr) {
            for (const auto& retry : chart_renderer.ConsumeHistoryRetries())
                application->RequestMarketHistory(retry);
        } else {
            static_cast<void>(chart_renderer.ConsumeHistoryRetries());
        }
        workspace.EndFrame();

        DrawImGuiDemo(show_imgui_demo,
                      static_cast<float>(chrome_metrics.title_bar_height));

        if (chart_renderer.ConsumePersistentChanges() ||
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
        if (!ResizeRenderer(renderer, width, height)) {
            done = true;
            continue;
        }
        const float clear_color[4] = {0.035f, 0.043f, 0.060f, 1.0f};
        renderer.context->OMSetRenderTargets(
            1, renderer.render_target.GetAddressOf(), nullptr);
        renderer.context->ClearRenderTargetView(
            renderer.render_target.Get(), clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        renderer.swap_chain->Present(1, 0);
    }

    // Ensure the background initializer has completed before its future and
    // the database-owning objects leave scope.
    if (!application && database_startup.valid()) {
        DatabaseStartupResult result = database_startup.get();
        if (!result.error.empty())
            MessageBoxA(nullptr, result.error.c_str(),
                        "Trade Box database error",
                        MB_OK | MB_ICONERROR);
        else {
            database = std::move(result.database);
            application = std::make_unique<
                tradebox::application::TradingApplication>(events, *database);
        }
    }

    CaptureNativeWindowState(window, workstation_state.native_window);
    std::string flush_error;
    if (!profile_store.Flush(workstation_state, flush_error)) {
        MessageBoxA(nullptr, flush_error.c_str(),
                    "Trade Box profile error", MB_OK | MB_ICONERROR);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
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
