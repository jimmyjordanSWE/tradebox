#include "tradebox/application/trading_application.h"
#include "tradebox/persistence/database.h"
#include "tradebox/ui/model.h"
#include "tradebox/ui/workspace.h"
#include "tradebox/workstation/profile_codec.h"
#include "tradebox/workstation/profile_store.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <string_view>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_sdl3.h"

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

struct LaunchOptions {
    int run_for_ms = 0;
    std::filesystem::path workspace_path;
    bool read_only_workspace = false;
};

struct DatabaseStartupResult {
    std::unique_ptr<Database> database;
    std::string error;
};

DatabaseStartupResult OpenDatabaseInBackground() {
    auto database = std::make_unique<Database>();
    std::string error;
    if (!database->Open(error))
        return {nullptr, std::move(error)};
    return {std::move(database), {}};
}

bool CreateRenderTarget(Dx11Renderer& renderer) {
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(renderer.swap_chain->GetBuffer(
            0, IID_PPV_ARGS(back_buffer.GetAddressOf()))))
        return false;
    return SUCCEEDED(renderer.device->CreateRenderTargetView(
        back_buffer.Get(), nullptr, renderer.render_target.GetAddressOf()));
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
    const HWND hwnd = reinterpret_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr));
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

void CaptureNativeWindowState(
    SDL_Window* window,
    tradebox::workstation::NativeWindowState& state) {
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    state.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    if (state.maximized || (flags & SDL_WINDOW_MINIMIZED) != 0) return;

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (SDL_GetWindowPosition(window, &x, &y) &&
        SDL_GetWindowSize(window, &width, &height)) {
        state.bounds = {static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(width),
                        static_cast<float>(height)};
    }
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
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowPosition(window,
                          static_cast<int>(native_window.bounds.x),
                          static_cast<int>(native_window.bounds.y));
    if (native_window.maximized) SDL_MaximizeWindow(window);

    Dx11Renderer renderer;
    if (!CreateRenderer(window, renderer)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().IniFilename = nullptr;
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

    tradebox::ui::UiScaleController ui_scale;
    tradebox::ui::Workspace workspace;
    workspace.SetPersistentState(&workstation_state.workspace);
    workspace.SetSnapPixels(
        workstation_state.application.window_snap_pixels);
    workspace.SetUiScale(workstation_state.application.ui_scale);
    ui_scale.SetScale(workstation_state.application.ui_scale);
    ui_scale.CaptureBaseline();
    if (!profile_existed) profile_store.MarkDirty();
    std::string last_profile_snapshot =
        tradebox::workstation::EncodeProfile(workstation_state);

    const Uint64 started_at = SDL_GetTicks();
    bool done = false;
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
                // Keep the GUI dependent on the application-owned snapshot
                // boundary while the visual surface is intentionally empty.
                const tradebox::application::UiSnapshotQuery empty_query;
                static_cast<void>(application->SnapshotForUi(empty_query));
                SDL_SetWindowTitle(window, "Trade Box");
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        if (ui_scale.HandleShortcuts()) {
            workspace.SetUiScale(ui_scale.Scale());
            workstation_state.application.ui_scale = ui_scale.Scale();
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        workspace.BeginFrame(viewport->Pos, viewport->Size, false);
        // Intentionally no windows, menus, controls, or overlays yet. New
        // surfaces should be created through Workspace so their state enters
        // the active .tbw profile automatically.
        workspace.EndFrame();

        CaptureNativeWindowState(window, workstation_state.native_window);
        workstation_state.application.window_snap_pixels =
            workspace.SnapPixels();
        const std::string profile_snapshot =
            tradebox::workstation::EncodeProfile(workstation_state);
        if (profile_snapshot != last_profile_snapshot) {
            profile_store.MarkDirty();
            last_profile_snapshot = profile_snapshot;
        }
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
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    return RunApplication(ParseLaunchOptions(argc, argv));
}
