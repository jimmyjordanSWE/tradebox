#include "tradebox/ui/workspace.h"

#include <gtest/gtest.h>

#include "imgui.h"

namespace tradebox::ui {
namespace {

class ImGuiContext final {
public:
    ImGuiContext() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().DisplaySize = {1000.0f, 800.0f};
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        int bytes_per_pixel = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(
            &pixels, &width, &height, &bytes_per_pixel);
    }

    ~ImGuiContext() { ImGui::DestroyContext(); }
};

TEST(WorkspaceDirty, StationaryWindowDoesNotDirtyPersistedState) {
    ImGuiContext context;
    workstation::WorkstationState state = workstation::WorkstationState::Defaults();
    state.workspace.windows.emplace(
        "tool.test",
        workstation::WindowInstanceState{
            .id = "tool.test",
            .kind = "tool",
            .title = "TEST",
            .open = true,
            .bounds = {24.0f, 24.0f, 420.0f, 280.0f}});

    Workspace workspace;
    workspace.SetPersistentState(&state.workspace);
    workspace.SetUiScale(1.0f);

    ImGui::NewFrame();
    workspace.BeginFrame({0.0f, 0.0f}, {1000.0f, 800.0f}, false);
    bool open = true;
    ASSERT_TRUE(workspace.BeginWindow("tool.test", "TEST", &open));
    workspace.EndWindow("tool.test");
    ImGui::Render();

    EXPECT_FALSE(workspace.ConsumeDirty());
}

TEST(WorkspaceDirty, ClosingWindowMarksPersistedStateDirty) {
    ImGuiContext context;
    workstation::WorkstationState state = workstation::WorkstationState::Defaults();
    state.workspace.windows.emplace(
        "tool.test",
        workstation::WindowInstanceState{
            .id = "tool.test",
            .kind = "tool",
            .title = "TEST",
            .open = true,
            .bounds = {24.0f, 24.0f, 420.0f, 280.0f}});

    Workspace workspace;
    workspace.SetPersistentState(&state.workspace);

    ImGui::NewFrame();
    workspace.BeginFrame({0.0f, 0.0f}, {1000.0f, 800.0f}, false);
    bool open = false;
    EXPECT_FALSE(workspace.BeginWindow("tool.test", "TEST", &open));
    ImGui::Render();

    EXPECT_TRUE(workspace.ConsumeDirty());
    EXPECT_FALSE(state.workspace.windows.at("tool.test").open);
}

TEST(WorkspaceWindow, RestoresClosedStateAndPairsEndSafely) {
    ImGuiContext context;
    workstation::WorkstationState state = workstation::WorkstationState::Defaults();
    state.workspace.windows.emplace(
        "tool.test",
        workstation::WindowInstanceState{
            .id = "tool.test",
            .kind = "tool",
            .title = "TEST",
            .open = false,
            .bounds = {24.0f, 24.0f, 420.0f, 280.0f}});

    Workspace workspace;
    workspace.SetPersistentState(&state.workspace);
    WorkspaceWindow window{
        .title = "TEST",
        .id = "tool.test",
    };

    ImGui::NewFrame();
    workspace.BeginFrame({0.0f, 0.0f}, {1000.0f, 800.0f}, false);
    EXPECT_FALSE(workspace.BeginWindow(window));
    EXPECT_FALSE(window.open);
    workspace.EndWindow(window);
    ImGui::Render();

    EXPECT_FALSE(workspace.ConsumeDirty());
}

TEST(WorkspaceDirty, PersistedScaleChangeMarksStateDirty) {
    ImGuiContext context;
    workstation::WorkstationState state = workstation::WorkstationState::Defaults();
    Workspace workspace;
    workspace.SetPersistentState(&state.workspace);

    workspace.SetUiScale(1.0f);
    EXPECT_FALSE(workspace.ConsumeDirty());

    workspace.SetUiScale(1.2f);
    EXPECT_TRUE(workspace.ConsumeDirty());
    EXPECT_FALSE(workspace.ConsumeDirty());
}

TEST(UiScaleController, ScalesFontsAndStyleFromCapturedBaseline) {
    ImGuiContext context;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = {8.0f, 8.0f};
    style.FontScaleMain = 1.0f;

    UiScaleController controller;
    controller.CaptureBaseline();
    controller.SetScale(1.5f);

    EXPECT_FLOAT_EQ(ImGui::GetStyle().FontScaleMain, 1.5f);
    EXPECT_FLOAT_EQ(ImGui::GetStyle().WindowPadding.x, 12.0f);
    EXPECT_FLOAT_EQ(ImGui::GetStyle().WindowPadding.y, 12.0f);
}

TEST(WorkspaceRecovery, ReturnsOversizedAndOffScreenWindowsToWorkArea) {
    ImGuiContext context;
    workstation::WorkstationState state = workstation::WorkstationState::Defaults();
    state.workspace.windows.emplace(
        "tool.offscreen",
        workstation::WindowInstanceState{
            .id = "tool.offscreen",
            .kind = "tool",
            .title = "OFFSCREEN",
            .open = true,
            .bounds = {1800.0f, 900.0f, 1200.0f, 700.0f}});
    state.workspace.windows.emplace(
        "tool.partial",
        workstation::WindowInstanceState{
            .id = "tool.partial",
            .kind = "tool",
            .title = "PARTIAL",
            .open = true,
            .bounds = {900.0f, 700.0f, 300.0f, 200.0f}});

    Workspace workspace;
    workspace.SetPersistentState(&state.workspace);
    workspace.BeginFrame({0.0f, 35.0f}, {1000.0f, 800.0f}, false);
    workspace.ReturnAllWindowsToWorkspace();

    const auto& offscreen = state.workspace.windows.at("tool.offscreen").bounds;
    EXPECT_FLOAT_EQ(offscreen.x, 0.0f);
    EXPECT_FLOAT_EQ(offscreen.y, 0.0f);
    EXPECT_FLOAT_EQ(offscreen.width, 1000.0f);
    EXPECT_FLOAT_EQ(offscreen.height, 700.0f);
    const auto& partial = state.workspace.windows.at("tool.partial").bounds;
    EXPECT_FLOAT_EQ(partial.x, 700.0f);
    EXPECT_FLOAT_EQ(partial.y, 600.0f);
    EXPECT_TRUE(workspace.ConsumeDirty());
}

TEST(WorkspaceRecovery, ScalesWorkAreaBeforeReturningWindows) {
    ImGuiContext context;
    workstation::WorkstationState state = workstation::WorkstationState::Defaults();
    state.workspace.windows.emplace(
        "tool.offscreen",
        workstation::WindowInstanceState{
            .id = "tool.offscreen",
            .kind = "tool",
            .title = "OFFSCREEN",
            .open = true,
            .bounds = {600.0f, 500.0f, 700.0f, 500.0f}});

    Workspace workspace;
    workspace.SetPersistentState(&state.workspace);
    workspace.SetUiScale(2.0f);
    static_cast<void>(workspace.ConsumeDirty());
    workspace.BeginFrame({0.0f, 35.0f}, {1000.0f, 800.0f}, false);
    workspace.ReturnAllWindowsToWorkspace();

    const auto& bounds = state.workspace.windows.at("tool.offscreen").bounds;
    EXPECT_FLOAT_EQ(bounds.x, 0.0f);
    EXPECT_FLOAT_EQ(bounds.y, 0.0f);
    EXPECT_FLOAT_EQ(bounds.width, 500.0f);
    EXPECT_FLOAT_EQ(bounds.height, 400.0f);
    EXPECT_TRUE(workspace.ConsumeDirty());
}

TEST(WorkspaceSnap, DropsWindowsOnTheGridOrExactWorkspaceEdge) {
    ImGuiContext context;
    workstation::WorkstationState state = workstation::WorkstationState::Defaults();
    state.workspace.windows.emplace(
        "tool.snap",
        workstation::WindowInstanceState{
            .id = "tool.snap",
            .kind = "tool",
            .title = "SNAP",
            .open = true,
            .bounds = {24.0f, 24.0f, 420.0f, 280.0f}});
    Workspace workspace;
    workspace.SetPersistentState(&state.workspace);
    workspace.SetSnapPixels(20);
    static_cast<void>(workspace.ConsumeDirty());

    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    ImGui::NewFrame();
    workspace.BeginFrame({0.0f, 0.0f}, {1000.0f, 800.0f}, true);
    bool open = true;
    ASSERT_TRUE(workspace.BeginWindow("tool.snap", "SNAP", &open));
    ImGui::SetWindowPos({575.0f, 145.0f});
    workspace.EndWindow("tool.snap");
    const auto& snapped_while_dragging =
        state.workspace.windows.at("tool.snap").bounds;
    EXPECT_FLOAT_EQ(snapped_while_dragging.x, 580.0f);
    EXPECT_FLOAT_EQ(snapped_while_dragging.y, 140.0f);
    ImGui::Render();

    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    ImGui::NewFrame();
    workspace.BeginFrame({0.0f, 0.0f}, {1000.0f, 800.0f}, true);
    ASSERT_TRUE(workspace.BeginWindow("tool.snap", "SNAP", &open));
    workspace.EndWindow("tool.snap");
    ImGui::Render();

    const auto& bounds = state.workspace.windows.at("tool.snap").bounds;
    EXPECT_FLOAT_EQ(bounds.x, 580.0f);
    EXPECT_FLOAT_EQ(bounds.y, 140.0f);
    EXPECT_TRUE(workspace.ConsumeDirty());
}

TEST(WorkspaceSnap, ResizesWindowsOnTheGridWhileDragging) {
    ImGuiContext context;
    workstation::WorkstationState state = workstation::WorkstationState::Defaults();
    state.workspace.windows.emplace(
        "tool.resize",
        workstation::WindowInstanceState{
            .id = "tool.resize",
            .kind = "tool",
            .title = "RESIZE",
            .open = true,
            .bounds = {24.0f, 24.0f, 420.0f, 280.0f}});
    Workspace workspace;
    workspace.SetPersistentState(&state.workspace);
    workspace.SetSnapPixels(20);
    static_cast<void>(workspace.ConsumeDirty());

    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    ImGui::NewFrame();
    workspace.BeginFrame({0.0f, 0.0f}, {1000.0f, 800.0f}, true);
    bool open = true;
    ASSERT_TRUE(workspace.BeginWindow("tool.resize", "RESIZE", &open));
    ImGui::SetWindowSize({575.0f, 345.0f}, ImGuiCond_Always);
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    workspace.EndWindow("tool.resize");

    const auto& snapped_while_resizing =
        state.workspace.windows.at("tool.resize").bounds;
    EXPECT_FLOAT_EQ(snapped_while_resizing.width, 580.0f);
    EXPECT_FLOAT_EQ(snapped_while_resizing.height, 340.0f);
    ImGui::Render();
}

}  // namespace
}  // namespace tradebox::ui
