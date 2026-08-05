#include "tradebox/application/trading_application.h"
#include "tradebox/persistence/database.h"
#include "tradebox/ui/model.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
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
};

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
        if (std::string_view(argv[index]) == "--run-for-ms" &&
            index + 1 < argc)
            options.run_for_ms = std::max(0, std::atoi(argv[++index]));
    }
    return options;
}

int RunApplication(const LaunchOptions& options) {
    Database database;
    std::string database_error;
    if (!database.Open(database_error)) {
        MessageBoxA(nullptr, database_error.c_str(), "Trade Box database error",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    UiEventQueue events;
    tradebox::application::TradingApplication application(events, database);
    // Keep the GUI dependent on the application-owned snapshot boundary while
    // the visual surface is intentionally empty.
    const tradebox::application::UiSnapshotQuery empty_query;
    static_cast<void>(application.SnapshotForUi(empty_query));

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return 1;
    SDL_Window* window = SDL_CreateWindow(
        "Trade Box", 960, 640,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
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
    ImGui::StyleColorsDark();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplSDL3_InitForD3D(window);
    ImGui_ImplDX11_Init(renderer.device.Get(), renderer.context.Get());

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

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        // Intentionally no windows, menus, controls, or overlays yet.
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
