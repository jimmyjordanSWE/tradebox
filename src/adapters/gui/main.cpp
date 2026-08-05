#include "tradebox/application/trading_application.h"
#include "tradebox/persistence/database.h"
#include "tradebox/ui/model.h"
#include "tradebox/workstation/profile_store.h"

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
#include <string>
#include <string_view>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_sdl3.h"

namespace {

using Microsoft::WRL::ComPtr;

constexpr float kTitleFontSize = 18.0f;
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

struct ChromeMetrics {
    int title_bar_height = 35;
    int control_width = 46;
    int tool_width = 40;
    int interactive_left_width = 0;
};

struct GuiFonts {
    ImFont* regular = nullptr;
    ImFont* title = nullptr;
    ImFont* icons = nullptr;
    std::array<ImWchar, 5> icon_ranges{
        0xe8b8, 0xe8b8, 0xf20b, 0xf20b, 0};
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
        AssetPath("fonts/B612-Regular.ttf").string().c_str(), 16.0f,
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
            metrics.control_width * 3 + metrics.tool_width * 2))
        return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

enum class ChromeButtonSymbol {
    Minimize,
    Maximize,
    Restore,
    Close,
};

void DrawChromeButtonSymbol(
    ImDrawList& draw_list, const ImVec2& button_min, const ImVec2& button_size,
    ChromeButtonSymbol symbol, ImU32 color) {
    constexpr float kSymbolSize = 10.0f;
    constexpr float kStrokeWidth = 1.0f;
    constexpr float kCornerRadius = 1.5f;
    constexpr float kSeparation = 2.0f;
    const float left = std::floor(
        button_min.x + (button_size.x - kSymbolSize) * 0.5f) + 0.5f;
    const float top = std::floor(
        button_min.y + (button_size.y - kSymbolSize) * 0.5f) + 0.5f;
    const float right = left + kSymbolSize - 1.0f;
    const float bottom = top + kSymbolSize - 1.0f;

    switch (symbol) {
        case ChromeButtonSymbol::Minimize: {
            const float middle = std::floor((top + bottom) * 0.5f) + 0.5f;
            draw_list.AddLine({left, middle}, {right, middle}, color,
                              kStrokeWidth);
            break;
        }
        case ChromeButtonSymbol::Maximize:
            draw_list.AddRect({left, top}, {right, bottom}, color,
                              kCornerRadius, ImDrawFlags_RoundCornersAll,
                              kStrokeWidth);
            break;
        case ChromeButtonSymbol::Restore:
            draw_list.AddLine(
                {left + kSeparation, top}, {right, top}, color, kStrokeWidth);
            draw_list.AddLine(
                {right, top}, {right, bottom - kSeparation}, color,
                kStrokeWidth);
            draw_list.AddRect(
                {left, top + kSeparation},
                {right - kSeparation, bottom}, color, kCornerRadius,
                ImDrawFlags_RoundCornersAll, kStrokeWidth);
            break;
        case ChromeButtonSymbol::Close:
            draw_list.AddLine({left, top}, {right, bottom}, color,
                              kStrokeWidth);
            draw_list.AddLine({right, top}, {left, bottom}, color,
                              kStrokeWidth);
            break;
    }
}

bool DrawChromeButton(
    const char* id, ChromeButtonSymbol symbol, ImVec2 size) {
    const bool clicked = ImGui::InvisibleButton(id, size);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool close = symbol == ChromeButtonSymbol::Close;

    ImDrawList& draw_list = *ImGui::GetWindowDrawList();
    if (hovered || active) {
        ImU32 background = 0;
        if (close) {
            background = active ? IM_COL32(232, 17, 35, 0x98)
                                : IM_COL32(232, 17, 35, 0xff);
        } else {
            const ImVec4 foreground = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            background = ImGui::ColorConvertFloat4ToU32(
                {foreground.x, foreground.y, foreground.z,
                 active ? 0x33 / 255.0f : 0x1a / 255.0f});
        }
        draw_list.AddRectFilled(minimum, maximum, background);
    }

    const ImU32 symbol_color = close && (hovered || active)
                                   ? IM_COL32(255, 255, 255, 255)
                                   : ImGui::GetColorU32(ImGuiCol_Text);
    DrawChromeButtonSymbol(
        draw_list, minimum, size, symbol, symbol_color);
    return clicked;
}

std::array<char, 4> Utf8BmpGlyph(unsigned int codepoint) {
    return {
        static_cast<char>(0xe0U | (codepoint >> 12U)),
        static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)),
        static_cast<char>(0x80U | (codepoint & 0x3fU)),
        '\0'};
}

void DrawTitleBarToolButton(
    const char* id, unsigned int codepoint,
    ImFont* icon_font, ImVec2 size) {
    static_cast<void>(ImGui::InvisibleButton(id, size));
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImDrawList& draw_list = *ImGui::GetWindowDrawList();

    if (hovered || active) {
        const ImVec4 foreground = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        draw_list.AddRectFilled(
            minimum, maximum,
            ImGui::ColorConvertFloat4ToU32(
                {foreground.x, foreground.y, foreground.z,
                 active ? 0x33 / 255.0f : 0x1a / 255.0f}));
    }

    const std::array<char, 4> glyph = Utf8BmpGlyph(codepoint);
    ImGui::PushFont(icon_font, kToolIconSize);
    const ImVec2 glyph_size = ImGui::CalcTextSize(glyph.data());
    draw_list.AddText(
        ImGui::GetFont(), ImGui::GetFontSize(),
        {minimum.x + (size.x - glyph_size.x) * 0.5f,
         minimum.y + (size.y - glyph_size.y) * 0.5f},
        ImGui::GetColorU32(ImGuiCol_Text), glyph.data());
    ImGui::PopFont();

}

void DrawApplicationChrome(
    SDL_Window* window, ChromeMetrics& metrics,
    const GuiFonts& fonts, const MarketTimeZone& market_time_zone,
    bool& done) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    metrics.title_bar_height = 35;
    metrics.control_width = 46;
    metrics.tool_width = 40;
    const float row_height = static_cast<float>(metrics.title_bar_height);
    const float caption_controls_width =
        static_cast<float>(metrics.control_width * 3);
    const float tool_controls_width =
        static_cast<float>(metrics.tool_width * 2);
    const float right_controls_width =
        tool_controls_width + caption_controls_width;
    metrics.interactive_left_width = 0;

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        {viewport->Size.x, row_height}, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("##tradebox_chrome", nullptr, flags)) {
        const ImVec2 window_min = ImGui::GetWindowPos();
        const ImVec2 window_size = ImGui::GetWindowSize();
        const ImVec2 window_max{window_min.x + window_size.x,
                                window_min.y + window_size.y};
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(
            window_min, window_max, ImGui::GetColorU32(ImGuiCol_MenuBarBg));
        draw_list->AddLine(
            {window_min.x, window_max.y - 1.0f},
            {window_max.x, window_max.y - 1.0f},
            ImGui::GetColorU32(ImGuiCol_Border));

        ImGui::SetCursorPos(
            {window_size.x - right_controls_width, 0.0f});
        const ImVec2 tool_size{
            static_cast<float>(metrics.tool_width), row_height};
        DrawTitleBarToolButton(
            "##account", 0xf20b, fonts.icons, tool_size);
        ImGui::SameLine(0.0f, 0.0f);
        DrawTitleBarToolButton(
            "##settings", 0xe8b8, fonts.icons, tool_size);
        ImGui::SameLine(0.0f, 0.0f);
        const ImVec2 button_size{
            static_cast<float>(metrics.control_width), row_height};
        if (DrawChromeButton(
                "##minimize", ChromeButtonSymbol::Minimize,
                button_size))
            static_cast<void>(SDL_MinimizeWindow(window));
        ImGui::SameLine(0.0f, 0.0f);
        const bool maximized =
            (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
        if (DrawChromeButton(
                "##maximize",
                maximized ? ChromeButtonSymbol::Restore
                          : ChromeButtonSymbol::Maximize,
                button_size)) {
            if (maximized)
                static_cast<void>(SDL_RestoreWindow(window));
            else
                static_cast<void>(SDL_MaximizeWindow(window));
        }
        ImGui::SameLine(0.0f, 0.0f);
        if (DrawChromeButton(
                "##close", ChromeButtonSymbol::Close,
                button_size))
            done = true;

        ImGui::PushFont(fonts.title, kTitleFontSize);
        const std::string title = MarketTimeText(market_time_zone);
        const ImVec2 title_size = ImGui::CalcTextSize(title.c_str());
        draw_list->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(),
            {window_min.x + (window_size.x - title_size.x) * 0.5f,
             window_min.y + (row_height - title_size.y) * 0.5f},
            ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
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
    ImGui::StyleColorsDark();
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

    if (!profile_existed) profile_store.MarkDirty();

    bool done = false;
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

        DrawApplicationChrome(
            window, chrome_metrics, gui_fonts, market_time_zone, done);

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
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    return RunApplication(ParseLaunchOptions(argc, argv));
}
