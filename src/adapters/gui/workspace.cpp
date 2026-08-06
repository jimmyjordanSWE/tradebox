#include "tradebox/ui/workspace.h"

#include <algorithm>
#include <cmath>

namespace tradebox::ui {
namespace {

bool Different(ImVec2 left, ImVec2 right) {
    return std::fabs(left.x - right.x) > 0.01f ||
           std::fabs(left.y - right.y) > 0.01f;
}

bool Different(const workstation::LogicalRect& left,
               const workstation::LogicalRect& right) {
    return std::fabs(left.x - right.x) > 0.25f ||
           std::fabs(left.y - right.y) > 0.25f ||
           std::fabs(left.width - right.width) > 0.25f ||
           std::fabs(left.height - right.height) > 0.25f;
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
    if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) {
        SetScale(1.0f);
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
        SetScale(scale_ + 0.1f);
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
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

void Workspace::SetPersistentState(workstation::WorkspaceState* state) {
    persistent_state_ = state;
}

void Workspace::BeginFrame(ImVec2 work_position, ImVec2 work_size,
                           bool snap_enabled) {
    work_position_ = work_position;
    work_size_ = ImVec2(std::max(0.0f, work_size.x), std::max(0.0f, work_size.y));
    snap_enabled_ = snap_enabled;
}

void Workspace::EndFrame() {}

void Workspace::SetUiScale(float scale) {
    const float next_scale = std::clamp(scale, 0.70f, 2.00f);
    if (std::fabs(ui_scale_ - next_scale) <= 0.001f) return;
    ui_scale_ = next_scale;
    dirty_ = true;
}

void Workspace::SetSnapPixels(int pixels) {
    const int next_pixels = std::clamp(pixels, 1, 1000);
    if (snap_pixels_ == next_pixels) return;
    snap_pixels_ = next_pixels;
    dirty_ = true;
}

void Workspace::ConstrainNextWindowSize(ImVec2 minimum, ImVec2 maximum) {
    ImGui::SetNextWindowSizeConstraints(minimum, maximum);
}

void Workspace::KeepWindowInsideWorkArea() {
    const ImVec2 window_size = ImGui::GetWindowSize();
    const ImVec2 current_position = ImGui::GetWindowPos();
    const ImVec2 minimum_position = work_position_;
    const ImVec2 maximum_position{
        std::max(minimum_position.x,
                 work_position_.x + work_size_.x - window_size.x),
        std::max(minimum_position.y,
                 work_position_.y + work_size_.y - window_size.y)};
    const ImVec2 constrained_position{
        std::clamp(current_position.x, minimum_position.x, maximum_position.x),
        std::clamp(current_position.y, minimum_position.y, maximum_position.y)};
    if (Different(current_position, constrained_position))
        ImGui::SetWindowPos(constrained_position, ImGuiCond_Always);
}

bool Workspace::BeginWindow(WorkspaceWindow& window) {
    window.began_this_frame = false;
    if (!window.initialized) {
        if (persistent_state_ != nullptr) {
            if (auto found = persistent_state_->windows.find(window.id);
                found != persistent_state_->windows.end())
                window.open = found->second.open;
        }
        window.initialized = true;
    }
    if (!window.open) {
        static_cast<void>(BeginWindow(window.id, window.title, &window.open,
                                      window.default_offset, window.default_size,
                                      window.flags));
        return false;
    }
    const bool visible = BeginWindow(window.id, window.title, &window.open,
                                     window.default_offset, window.default_size,
                                     window.flags);
    window.began_this_frame = true;
    return visible;
}

bool Workspace::BeginWindow(std::string_view id, std::string_view title, bool* open,
                            ImVec2 default_offset, ImVec2 default_size,
                            ImGuiWindowFlags flags) {
    if (open == nullptr) return false;
    if (!*open) {
        if (persistent_state_ != nullptr) {
            if (auto found = persistent_state_->windows.find(std::string(id));
                found != persistent_state_->windows.end() && found->second.open) {
                found->second.open = false;
                dirty_ = true;
            }
        }
        return false;
    }
    workstation::WindowInstanceState local{
        .id = std::string(id), .kind = "tool", .title = std::string(title), .open = *open,
        .bounds = {default_offset.x, default_offset.y, default_size.x, default_size.y}};
    workstation::WindowInstanceState* state = &local;
    if (persistent_state_ != nullptr) {
        auto [found, inserted] = persistent_state_->windows.try_emplace(std::string(id), local);
        if (inserted) dirty_ = true;
        state = &found->second;
        const std::string next_title(title);
        if (state->title != next_title) {
            state->title = next_title;
            dirty_ = true;
        }
        if (state->open != *open) {
            state->open = *open;
            dirty_ = true;
        }
    }
    if (!state->open) return false;
    ImGui::SetNextWindowPos(
        ImVec2(work_position_.x + state->bounds.x * ui_scale_,
               work_position_.y + state->bounds.y * ui_scale_), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(state->bounds.width * ui_scale_,
                                    state->bounds.height * ui_scale_),
                             ImGuiCond_FirstUseEver);
    const std::string label = std::string(title) + "###" + std::string(id);
    const bool open_before = state->open;
    const bool visible = ImGui::Begin(label.c_str(), &state->open, flags);
    KeepWindowInsideWorkArea();
    if (state->open != open_before) dirty_ = true;
    *open = state->open;
    return visible;
}

void Workspace::EndWindow(std::string_view id) {
    if (persistent_state_ != nullptr) {
        if (auto found = persistent_state_->windows.find(std::string(id));
            found != persistent_state_->windows.end()) {
            const ImVec2 position = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            auto& state = found->second;
            const workstation::LogicalRect next_bounds{
                (position.x - work_position_.x) / ui_scale_,
                (position.y - work_position_.y) / ui_scale_,
                size.x / ui_scale_,
                size.y / ui_scale_};
            if (Different(state.bounds, next_bounds)) {
                state.bounds = next_bounds;
                dirty_ = true;
            }
        }
    }
    ImGui::End();
}

void Workspace::EndWindow(WorkspaceWindow& window) {
    if (!window.began_this_frame) return;
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    if (persistent_state_ != nullptr) {
        auto found = persistent_state_->windows.find(window.id);
        if (found != persistent_state_->windows.end()) {
            auto& state = found->second;
            const workstation::LogicalRect next_bounds{
                (position.x - work_position_.x) / ui_scale_,
                (position.y - work_position_.y) / ui_scale_,
                size.x / ui_scale_,
                size.y / ui_scale_};
            if (Different(state.bounds, next_bounds)) {
                state.bounds = next_bounds;
                dirty_ = true;
            }
            if (state.open != window.open) {
                state.open = window.open;
                dirty_ = true;
            }
        }
    }
    ImGui::End();
    window.began_this_frame = false;
}

void Workspace::ResetWindow(WorkspaceWindow& window) {
    window.initialized = false;
    window.reset_geometry = true;
}

void Workspace::ResetAll() {
    if (persistent_state_ == nullptr) return;
    const auto defaults = workstation::WorkstationState::Defaults().workspace.windows;
    for (auto& [id, window] : persistent_state_->windows) {
        if (const auto default_window = defaults.find(id);
            default_window != defaults.end()) {
            const bool open = window.open;
            if (Different(window.bounds, default_window->second.bounds)) {
                window.bounds = default_window->second.bounds;
                dirty_ = true;
            }
            if (window.open != open) window.open = open;
        }
    }
}

ImVec2 Workspace::SnapPosition(ImVec2 position, ImVec2 window_size) const {
    const float step = static_cast<float>(snap_pixels_);
    const auto snap_axis = [step](float value, float origin, float canvas_length, float size) {
        const float available = std::max(0.0f, canvas_length - size);
        const int last_cell = static_cast<int>(std::floor(available / step));
        const int cell = std::clamp(static_cast<int>(std::lround((value - origin) / step)),
                                    0, last_cell);
        return origin + static_cast<float>(cell) * step;
    };
    return {snap_axis(position.x, work_position_.x, work_size_.x, window_size.x),
            snap_axis(position.y, work_position_.y, work_size_.y, window_size.y)};
}

}  // namespace tradebox::ui
