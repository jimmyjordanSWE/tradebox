#include "tradebox/ui/workspace.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <string>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

namespace {

struct DummyUi {
    tradebox::ui::Workspace workspace;
    tradebox::ui::UiScaleController scale;
    tradebox::ui::WorkspaceWindow charts{
        .title = "CHARTS", .id = "charts",
        .default_offset = ImVec2(20.0f, 20.0f),
        .default_size = ImVec2(760.0f, 520.0f),
    };
    tradebox::ui::WorkspaceWindow market{
        .title = "MARKET", .id = "market",
        .default_offset = ImVec2(800.0f, 20.0f),
        .default_size = ImVec2(520.0f, 360.0f),
    };
    tradebox::ui::WorkspaceWindow order{
        .title = "ORDER", .id = "order",
        .default_offset = ImVec2(800.0f, 400.0f),
        .default_size = ImVec2(360.0f, 340.0f),
    };
    bool snap_enabled = true;
    bool reset_requested = false;
    int selected_chart = 0;
};

bool BeginDummyWindow(tradebox::ui::Workspace& workspace,
                      tradebox::ui::WorkspaceWindow& window) {
    if (!window.open) return false;
    const bool visible = workspace.BeginWindow(window);
    if (!visible) workspace.EndWindow(window);
    return visible;
}

void DrawControls(DummyUi& ui, ImVec2 work_position, ImVec2 work_size) {
    const float scale = ui.scale.Scale();
    const float control_height = 40.0f * scale;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos,
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(ImGui::GetMainViewport()->WorkSize.x, control_height),
        ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("DUMMY CONTROLS###dummy-controls", nullptr, flags);
    ImGui::TextUnformatted("TradeBox dummy workspace");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox("Snap to workspace edges", &ui.snap_enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("hold Ctrl while moving to force snapping");
    ImGui::SameLine();
    if (ImGui::Button("Reset layout")) ui.reset_requested = true;
    ImGui::SameLine();
    ImGui::TextDisabled("workspace %.0fx%.0f at %.0f,%.0f", work_size.x,
                        work_size.y, work_position.x, work_position.y);
    ImGui::End();
}

void DrawCharts(DummyUi& ui) {
    if (!BeginDummyWindow(ui.workspace, ui.charts)) return;
    ImGui::TextDisabled("Generic resizable window host");
    ImGui::Separator();
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            const int index = row * 4 + column;
            ImGui::PushID(index);
            const std::string label = "Chart " + std::to_string(index + 1);
            const float scale = ui.scale.Scale();
            if (ImGui::Button(label.c_str(),
                              ImVec2(132.0f * scale, 84.0f * scale)))
                ui.selected_chart = index;
            if (column != 3) ImGui::SameLine();
            ImGui::PopID();
        }
    }
    ImGui::Text("Selected demo chart: %d", ui.selected_chart + 1);
    ui.workspace.EndWindow(ui.charts);
}

void DrawMarket(DummyUi& ui) {
    if (!BeginDummyWindow(ui.workspace, ui.market)) return;
    ImGui::TextDisabled("Placeholder scanner window");
    if (ImGui::BeginTable("dummy-market-table", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                              ImGuiTableFlags_SizingStretchProp)) {
        for (const char* column : {"Symbol", "Last", "Change %", "Status"})
            ImGui::TableSetupColumn(column);
        ImGui::TableHeadersRow();
        for (const char* symbol : {"AMD", "MSFT", "AAPL", "NVDA", "TSLA",
                                   "QQQ"}) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Selectable(symbol, false,
                              ImGuiSelectableFlags_SpanAllColumns);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("dummy");
        }
        ImGui::EndTable();
    }
    ui.workspace.EndWindow(ui.market);
}

void DrawOrder(DummyUi& ui) {
    if (!BeginDummyWindow(ui.workspace, ui.order)) return;
    ImGui::TextUnformatted("Order window placeholder");
    ImGui::Text("Selected chart: %d", ui.selected_chart + 1);
    ImGui::TextDisabled("This window is intentionally disconnected from core.");
    ui.workspace.EndWindow(ui.order);
}

int Run() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);

    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                          "TradeBox Dummy UI");
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
                          1440);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                          900);
    SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN,
                           true);
    SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN,
                           true);
    SDL_Window* window = SDL_CreateWindowWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (!window) {
        SDL_Quit();
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().FontSizeBase = 14.0f;
    ImGui::GetStyle().FontScaleDpi = SDL_GetWindowDisplayScale(window);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "dummy-ui-layout.ini";
    ImGui_ImplSDL3_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 150");

    DummyUi ui;
    ui.scale.CaptureBaseline();
    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                done = true;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ui.scale.HandleShortcuts();
        ui.workspace.SetUiScale(ui.scale.Scale());

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float control_gap = 6.0f * ui.scale.Scale();
        const ImVec2 work_position(viewport->WorkPos.x,
                                   viewport->WorkPos.y +
                                       40.0f * ui.scale.Scale() + control_gap);
        const ImVec2 work_size(viewport->WorkSize.x,
                               std::max(0.0f, viewport->WorkSize.y -
                                                   40.0f * ui.scale.Scale() -
                                                   control_gap));
        DrawControls(ui, work_position, work_size);
        ui.workspace.BeginFrame(work_position, work_size, ui.snap_enabled);
        if (ui.reset_requested) {
            ui.workspace.ResetWindow(ui.charts);
            ui.workspace.ResetWindow(ui.market);
            ui.workspace.ResetWindow(ui.order);
            ui.reset_requested = false;
        }
        DrawCharts(ui);
        DrawMarket(ui);
        DrawOrder(ui);
        ui.workspace.EndFrame();

        ImGui::Render();
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.035f, 0.043f, 0.060f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

}  // namespace

int main(int, char**) { return Run(); }
