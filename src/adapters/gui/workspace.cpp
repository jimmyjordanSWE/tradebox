#include "tradebox/ui/workspace.h"

#include <algorithm>
#include <cmath>

namespace tradebox::ui {
namespace {

bool Different(ImVec2 left, ImVec2 right) {
    return std::fabs(left.x - right.x) > 0.01f ||
           std::fabs(left.y - right.y) > 0.01f;
}

bool Contains(const std::vector<std::string>& values,
              const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool RangesOverlap(float first_min, float first_max, float second_min,
                   float second_max, float padding) {
    return first_max + padding >= second_min &&
           second_max + padding >= first_min;
}

}  // namespace

void UiScaleController::CaptureBaseline() {
    baseline_ = ImGui::GetStyle();
    captured_ = true;
    SetScale(scale_);
}

bool UiScaleController::HandleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.KeyCtrl) return false;

    if (ImGui::IsKeyPressed(ImGuiKey_0) ||
        ImGui::IsKeyPressed(ImGuiKey_Keypad0)) {
        SetScale(1.0f);
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
        SetScale(scale_ + 0.1f);
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
        SetScale(scale_ - 0.1f);
        return true;
    }
    return false;
}

void UiScaleController::SetScale(float scale) {
    scale_ = std::clamp(scale, 0.70f, 2.00f);
    if (!captured_) return;

    ImGuiStyle scaled = baseline_;
    scaled.ScaleAllSizes(scale_);
    scaled.FontScaleMain = baseline_.FontScaleMain * scale_;
    ImGui::GetStyle() = scaled;
}

void Workspace::BeginFrame(ImVec2 work_position, ImVec2 work_size,
                           bool snap_enabled) {
    work_position_ = work_position;
    work_size_ = ImVec2(std::max(0.0f, work_size.x),
                        std::max(0.0f, work_size.y));
    snap_enabled_ = snap_enabled;

    known_windows_.erase(
        std::remove_if(
            known_windows_.begin(), known_windows_.end(),
            [this](const WorkspaceWindow& window) {
                return !Contains(previous_window_ids_, window.id);
            }),
        known_windows_.end());
    current_window_ids_.clear();

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImVec2 work_end(work_position_.x + work_size_.x,
                          work_position_.y + work_size_.y);
    draw->AddRectFilled(work_position_, work_end, IM_COL32(10, 14, 21, 255));
    draw->AddRect(work_position_, work_end, IM_COL32(55, 68, 88, 255), 0.0f,
                  0, 1.0f);
}

void Workspace::SetUiScale(float scale) {
    ui_scale_ = std::clamp(scale, 0.70f, 2.00f);
}

bool Workspace::BeginWindow(WorkspaceWindow& window) {
    if (!window.open) return false;

    if (!Contains(current_window_ids_, window.id))
        current_window_ids_.push_back(window.id);

    if (!window.initialized) {
        ImGui::SetNextWindowPos(
            ImVec2(work_position_.x + window.default_offset.x * ui_scale_,
                   work_position_.y + window.default_offset.y * ui_scale_),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(window.default_size.x * ui_scale_,
                   window.default_size.y * ui_scale_),
            ImGuiCond_Always);
        window.initialized = true;
    } else if (window.has_pending_position) {
        ImGui::SetNextWindowPos(window.pending_position, ImGuiCond_Always);
        window.has_pending_position = false;
    }

    const std::string label = window.title + "###" + window.id;
    return ImGui::Begin(label.c_str(), &window.open);
}

void Workspace::EndWindow(WorkspaceWindow& window) {
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImGuiIO& io = ImGui::GetIO();
    const bool focused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool dragging =
        focused && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    const bool position_changed =
        !window.has_last_position || Different(position, window.last_position);

    if (dragging && io.KeyCtrl) window.snap_modifier_seen = true;
    if (window.was_dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        position_changed && ((snap_enabled_ && window.snap_enabled) ||
                             window.snap_modifier_seen)) {
        const ImVec2 snapped = SnapPosition(window.id, position, size);
        if (Different(position, snapped)) {
            window.pending_position = snapped;
            window.has_pending_position = true;
        }
        window.snap_modifier_seen = false;
    }

    window.was_dragging = dragging;
    window.last_position = position;
    window.last_size = size;
    window.has_last_position = true;

    const auto known = std::find_if(
        known_windows_.begin(), known_windows_.end(),
        [&window](const WorkspaceWindow& candidate) {
            return candidate.id == window.id;
        });
    if (known == known_windows_.end())
        known_windows_.push_back(window);
    else
        *known = window;
    ImGui::End();
}

void Workspace::EndFrame() {
    previous_window_ids_ = current_window_ids_;
}

void Workspace::ResetWindow(WorkspaceWindow& window) {
    window.initialized = false;
    window.was_dragging = false;
    window.snap_modifier_seen = false;
    window.has_last_position = false;
    window.last_size = ImVec2(0.0f, 0.0f);
    window.has_pending_position = false;
}

ImVec2 Workspace::SnapPosition(const std::string& current_id,
                               ImVec2 position, ImVec2 window_size) const {
    const float left = work_position_.x;
    const float top = work_position_.y;
    const float right = work_position_.x + work_size_.x;
    const float bottom = work_position_.y + work_size_.y;

    float x_delta = snap_distance_ + 0.01f;
    float y_delta = snap_distance_ + 0.01f;
    auto consider_x = [&x_delta](float current_edge, float target_edge) {
        const float delta = target_edge - current_edge;
        if (std::fabs(delta) <= std::fabs(x_delta)) x_delta = delta;
    };
    auto consider_y = [&y_delta](float current_edge, float target_edge) {
        const float delta = target_edge - current_edge;
        if (std::fabs(delta) <= std::fabs(y_delta)) y_delta = delta;
    };

    consider_x(position.x, left);
    consider_x(position.x + window_size.x, right);
    consider_y(position.y, top);
    consider_y(position.y + window_size.y, bottom);

    for (const WorkspaceWindow& other : known_windows_) {
        if (other.id == current_id || !other.open ||
            !other.has_last_position)
            continue;

        const ImVec2 other_position = other.last_position;
        const ImVec2 other_size = other.last_size;
        const float other_right = other_position.x + other_size.x;
        const float other_bottom = other_position.y + other_size.y;
        if (RangesOverlap(position.y, position.y + window_size.y,
                          other_position.y, other_bottom, snap_distance_)) {
            consider_x(position.x, other_position.x);
            consider_x(position.x, other_right);
            consider_x(position.x + window_size.x, other_position.x);
            consider_x(position.x + window_size.x, other_right);
        }
        if (RangesOverlap(position.x, position.x + window_size.x,
                          other_position.x, other_right, snap_distance_)) {
            consider_y(position.y, other_position.y);
            consider_y(position.y, other_bottom);
            consider_y(position.y + window_size.y, other_position.y);
            consider_y(position.y + window_size.y, other_bottom);
        }
    }

    if (std::fabs(x_delta) <= snap_distance_) position.x += x_delta;
    if (std::fabs(y_delta) <= snap_distance_) position.y += y_delta;
    return position;
}

}  // namespace tradebox::ui
