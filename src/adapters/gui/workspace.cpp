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

WorkspaceWindow* FindWindow(std::vector<WorkspaceWindow>& windows,
                            std::string_view id) {
    const auto found = std::find_if(
        windows.begin(), windows.end(),
        [id](const WorkspaceWindow& window) { return window.id == id; });
    return found == windows.end() ? nullptr : &*found;
}

bool IsResizeCursor(ImGuiMouseCursor cursor) {
    return cursor == ImGuiMouseCursor_ResizeNS ||
           cursor == ImGuiMouseCursor_ResizeEW ||
           cursor == ImGuiMouseCursor_ResizeNESW ||
           cursor == ImGuiMouseCursor_ResizeNWSE;
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

}

void Workspace::SetUiScale(float scale) {
    ui_scale_ = std::clamp(scale, 0.70f, 2.00f);
}

void Workspace::SetSnapPixels(int pixels) {
    snap_pixels_ = std::clamp(pixels, 1, 1'000);
}

void Workspace::ConstrainNextWindowSize(ImVec2 minimum, ImVec2 maximum) {
    // Let Dear ImGui own interactive resizing. The workspace may constrain a
    // window's legal range, but it must not rewrite the size selected by the
    // user while the native resize interaction is in progress.
    ImGui::SetNextWindowSizeConstraints(minimum, maximum);
}

bool Workspace::BeginWindow(WorkspaceWindow& window) {
    if (!window.open) return false;

    if (!Contains(current_window_ids_, window.id))
        current_window_ids_.push_back(window.id);

    if (!window.initialized) {
        const ImGuiCond geometry_condition =
            window.reset_geometry ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImGui::SetNextWindowPos(
            ImVec2(work_position_.x + window.default_offset.x * ui_scale_,
                   work_position_.y + window.default_offset.y * ui_scale_),
            geometry_condition);
        ImGui::SetNextWindowSize(
            ImVec2(window.default_size.x * ui_scale_,
                   window.default_size.y * ui_scale_),
            geometry_condition);
        window.initialized = true;
        window.reset_geometry = false;
    } else if (window.has_pending_position) {
        ImGui::SetNextWindowPos(window.pending_position, ImGuiCond_Always);
        window.has_pending_position = false;
    }

    const std::string label = window.title + "###" + window.id;
    return ImGui::Begin(label.c_str(), &window.open);
}

bool Workspace::BeginWindow(std::string_view id, std::string_view title,
                            bool* open, ImVec2 default_offset,
                            ImVec2 default_size) {
    if (open == nullptr) return false;
    WorkspaceWindow* window = FindWindow(known_windows_, id);
    if (window == nullptr) {
        known_windows_.push_back(WorkspaceWindow{
            .title = std::string(title),
            .id = std::string(id),
            .default_offset = default_offset,
            .default_size = default_size,
        });
        window = &known_windows_.back();
    }
    window->title = std::string(title);
    window->default_offset = default_offset;
    window->default_size = default_size;
    window->open_binding = open;
    window->open = *open;
    if (!window->open) return false;
    const bool visible = BeginWindow(*window);
    if (!visible) *open = window->open;
    return visible;
}

void Workspace::EndWindow(std::string_view id) {
    const std::string window_id(id);
    if (!Contains(current_window_ids_, window_id))
        current_window_ids_.push_back(window_id);

    auto known = std::find_if(
        known_windows_.begin(), known_windows_.end(),
        [&window_id](const WorkspaceWindow& candidate) {
            return candidate.id == window_id;
        });
    if (known == known_windows_.end()) {
        known_windows_.push_back(WorkspaceWindow{
            .title = window_id,
            .id = window_id,
        });
        known = known_windows_.end() - 1;
    }
    EndWindow(*known);
}

void Workspace::EndWindow(WorkspaceWindow& window) {
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImGuiIO& io = ImGui::GetIO();
    const bool focused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool dragging =
        focused && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    const bool resizing = dragging && IsResizeCursor(ImGui::GetMouseCursor());
    const bool moving = dragging && !resizing;
    const bool position_changed =
        !window.has_last_position || Different(position, window.last_position);

    if (moving && io.KeyCtrl) window.snap_modifier_seen = true;

    // Snap only after a move completes. Applying SetWindowPos every frame
    // while the mouse is down interferes with native ImGui resize handling.
    if (window.was_dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        position_changed &&
        ((snap_enabled_ && window.snap_enabled) ||
         window.snap_modifier_seen)) {
        const ImVec2 snapped = SnapPosition(position, size);
        if (Different(position, snapped)) {
            window.pending_position = snapped;
            window.has_pending_position = true;
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        window.snap_modifier_seen = false;

    window.was_dragging = moving;
    window.was_resizing = resizing;
    window.last_position = position;
    window.last_size = size;
    window.has_last_position = true;
    if (window.open_binding != nullptr)
        *window.open_binding = window.open;

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
    // The next BeginWindow uses ImGuiCond_Always once, which intentionally
    // overrides any persisted geometry for an explicit layout reset. Normal
    // startup uses ImGuiCond_FirstUseEver so persisted user geometry wins.
    window.initialized = false;
    window.reset_geometry = true;
    window.was_dragging = false;
    window.was_resizing = false;
    window.snap_modifier_seen = false;
    window.has_last_position = false;
    window.last_size = ImVec2(0.0f, 0.0f);
    window.has_pending_position = false;
}

void Workspace::ResetAll() {
    for (WorkspaceWindow& window : known_windows_)
        ResetWindow(window);
}

ImVec2 Workspace::SnapPosition(ImVec2 position,
                               ImVec2 window_size) const {
    const float left = work_position_.x;
    const float top = work_position_.y;
    const float step = static_cast<float>(snap_pixels_);
    const auto snap_axis = [step](float value, float origin,
                                  float canvas_length,
                                  float size) {
        const float available = std::max(0.0f, canvas_length - size);
        const int last_cell = static_cast<int>(std::floor(available / step));
        const int cell = std::clamp(
            static_cast<int>(std::lround((value - origin) / step)), 0,
            last_cell);
        return origin + static_cast<float>(cell) * step;
    };
    position.x = snap_axis(position.x, left, work_size_.x, window_size.x);
    position.y = snap_axis(position.y, top, work_size_.y, window_size.y);
    return position;
}

}  // namespace tradebox::ui
