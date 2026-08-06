#pragma once

#include "tradebox/workstation/state.h"
#include "imgui.h"

#include <cfloat>
#include <string>
#include <string_view>
#include <vector>

namespace tradebox::ui {

// A window model deliberately independent of trading/core state. The same
// wrapper can host charts, tables, order entry, or diagnostic content.
struct WorkspaceWindow {
    std::string title;
    std::string id;
    ImVec2 default_offset{24.0f, 24.0f};
    ImVec2 default_size{420.0f, 280.0f};
    bool open = true;
    bool* open_binding = nullptr;
    bool snap_enabled = true;
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;

    bool initialized = false;
    bool reset_geometry = false;
    bool was_dragging = false;
    bool was_resizing = false;
    bool snap_modifier_seen = false;
    bool has_last_position = false;
    bool has_pending_position = false;
    ImVec2 last_position{};
    ImVec2 last_size{};
    ImVec2 pending_position{};
    bool began_this_frame = false;
};

class UiScaleController {
public:
    void CaptureBaseline();
    bool HandleShortcuts();
    void SetScale(float scale);

    [[nodiscard]] float Scale() const { return scale_; }

private:
    ImGuiStyle baseline_{};
    float scale_ = 1.0f;
    bool captured_ = false;
};

class Workspace {
public:
    void BeginFrame(ImVec2 work_position, ImVec2 work_size,
                    bool snap_enabled);
    void SetPersistentState(workstation::WorkspaceState* state);
    void EndFrame();
    void SetUiScale(float scale);
    void SetSnapPixels(int pixels);
    void ConstrainNextWindowSize(ImVec2 minimum = ImVec2(0.0f, 0.0f),
                                 ImVec2 maximum = ImVec2(FLT_MAX, FLT_MAX));
    [[nodiscard]] bool BeginWindow(WorkspaceWindow& window);
    [[nodiscard]] bool BeginWindow(std::string_view id,
                                   std::string_view title,
                                   bool* open,
                                   ImVec2 default_offset = ImVec2(24.0f, 24.0f),
                                   ImVec2 default_size = ImVec2(420.0f, 280.0f),
                                   ImGuiWindowFlags flags = ImGuiWindowFlags_None);
    void EndWindow(std::string_view id);
    void EndWindow(WorkspaceWindow& window);
    void ResetWindow(WorkspaceWindow& window);
    void ResetAll();

    void MarkDirty() { dirty_ = true; }
    [[nodiscard]] bool ConsumeDirty() {
        const bool dirty = dirty_;
        dirty_ = false;
        return dirty;
    }

    [[nodiscard]] int SnapPixels() const { return snap_pixels_; }

private:
    void KeepWindowInsideWorkArea();
    [[nodiscard]] ImVec2 SnapPosition(ImVec2 position,
                                      ImVec2 window_size) const;

    ImVec2 work_position_{};
    ImVec2 work_size_{};
    int snap_pixels_ = 10;
    bool snap_enabled_ = true;
    float ui_scale_ = 1.0f;
    bool dirty_ = false;
    workstation::WorkspaceState* persistent_state_ = nullptr;
};

}  // namespace tradebox::ui
