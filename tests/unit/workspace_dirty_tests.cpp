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

}  // namespace
}  // namespace tradebox::ui
