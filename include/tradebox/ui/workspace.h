#pragma once

#include "imgui.h"

#include <string>
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
    bool snap_enabled = true;

    bool initialized = false;
    bool was_dragging = false;
    bool snap_modifier_seen = false;
    bool has_last_position = false;
    bool has_pending_position = false;
    ImVec2 last_position{};
    ImVec2 last_size{};
    ImVec2 pending_position{};
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
    explicit Workspace(float snap_distance = 10.0f)
        : snap_distance_(snap_distance) {}

    void BeginFrame(ImVec2 work_position, ImVec2 work_size,
                    bool snap_enabled);
    void EndFrame();
    void SetUiScale(float scale);
    [[nodiscard]] bool BeginWindow(WorkspaceWindow& window);
    void EndWindow(WorkspaceWindow& window);
    void ResetWindow(WorkspaceWindow& window);

    [[nodiscard]] float SnapDistance() const { return snap_distance_; }

private:
    [[nodiscard]] ImVec2 SnapPosition(const std::string& current_id,
                                      ImVec2 position,
                                      ImVec2 window_size) const;

    ImVec2 work_position_{};
    ImVec2 work_size_{};
    float snap_distance_ = 10.0f;
    bool snap_enabled_ = true;
    float ui_scale_ = 1.0f;
    std::vector<WorkspaceWindow> known_windows_;
    std::vector<std::string> current_window_ids_;
    std::vector<std::string> previous_window_ids_;
};

}  // namespace tradebox::ui
