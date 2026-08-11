#include "tradebox/ui/workspace.h"

#include <algorithm>
#include <cmath>
#include <utility>

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
    next_minimum_size_ = minimum;
    next_maximum_size_ = maximum;
    has_next_size_constraints_ = true;
}

bool Workspace::NormalizeBounds(workstation::LogicalRect& bounds) const {
    const float available_width = std::max(1.0f, work_size_.x / ui_scale_);
    const float available_height = std::max(1.0f, work_size_.y / ui_scale_);
    const workstation::LogicalRect original = bounds;

    bounds.width = std::clamp(bounds.width,
                              std::min(160.0f, available_width),
                              available_width);
    bounds.height = std::clamp(bounds.height,
                               std::min(100.0f, available_height),
                               available_height);
    const bool fully_outside = bounds.x >= available_width ||
                               bounds.y >= available_height ||
                               bounds.x + bounds.width <= 0.0f ||
                               bounds.y + bounds.height <= 0.0f;
    if (fully_outside) {
        bounds.x = 0.0f;
        bounds.y = 0.0f;
    } else {
        bounds.x = std::clamp(bounds.x, 0.0f, available_width - bounds.width);
        bounds.y = std::clamp(bounds.y, 0.0f, available_height - bounds.height);
    }
    return Different(original, bounds);
}

void Workspace::ApplyNextWindowSizeConstraints(std::string_view id) {
    const ImVec2 requested_minimum = has_next_size_constraints_
                                         ? next_minimum_size_
                                         : ImVec2(0.0f, 0.0f);
    const ImVec2 requested_maximum = has_next_size_constraints_
                                         ? next_maximum_size_
                                         : ImVec2(FLT_MAX, FLT_MAX);
    has_next_size_constraints_ = false;

    const auto constrained_axis = [this](float minimum, float maximum,
                                         float available) {
        const float physical_available = std::max(1.0f, available);
        const float physical_minimum =
            std::max(0.0f, minimum) * ui_scale_;
        const float physical_maximum = maximum >= FLT_MAX
                                           ? physical_available
                                           : std::max(0.0f, maximum) * ui_scale_;
        const float final_maximum = std::min(physical_maximum, physical_available);
        return std::pair{std::min(physical_minimum, final_maximum),
                         final_maximum};
    };
    const auto width = constrained_axis(requested_minimum.x, requested_maximum.x,
                                        work_size_.x);
    const auto height = constrained_axis(requested_minimum.y, requested_maximum.y,
                                         work_size_.y);
    minimum_sizes_[std::string(id)] = {width.first / ui_scale_,
                                       height.first / ui_scale_};
    ImGui::SetNextWindowSizeConstraints({width.first, height.first},
                                        {width.second, height.second});
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
    if (open == nullptr) {
        has_next_size_constraints_ = false;
        return false;
    }
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
    if (!state->open) {
        has_next_size_constraints_ = false;
        return false;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        NormalizeBounds(state->bounds)) {
        pending_geometry_.insert(state->id);
        dirty_ = true;
    }
    ApplyNextWindowSizeConstraints(state->id);
    const bool apply_pending_geometry =
        pending_geometry_.erase(state->id) != 0U;
    ImGui::SetNextWindowPos(
        ImVec2(work_position_.x + state->bounds.x * ui_scale_,
               work_position_.y + state->bounds.y * ui_scale_),
        apply_pending_geometry ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(state->bounds.width * ui_scale_,
                                    state->bounds.height * ui_scale_),
                             apply_pending_geometry ? ImGuiCond_Always
                                                    : ImGuiCond_FirstUseEver);
    const std::string label = std::string(title) + "###" + std::string(id);
    const bool open_before = state->open;
    const bool visible = ImGui::Begin(label.c_str(), &state->open, flags);
    if (state->open != open_before) dirty_ = true;
    *open = state->open;
    return visible;
}

void Workspace::EndWindow(std::string_view id) {
    PersistWindowBounds(id);
    ImGui::End();
}

void Workspace::EndWindow(WorkspaceWindow& window) {
    if (!window.began_this_frame) return;
    PersistWindowBounds(window.id);
    if (persistent_state_ != nullptr) {
        auto found = persistent_state_->windows.find(window.id);
        if (found != persistent_state_->windows.end()) {
            auto& state = found->second;
            if (state.open != window.open) {
                state.open = window.open;
                dirty_ = true;
            }
        }
    }
    ImGui::End();
    window.began_this_frame = false;
}

void Workspace::PersistWindowBounds(std::string_view id) {
    if (persistent_state_ == nullptr) return;
    const auto found = persistent_state_->windows.find(std::string(id));
    if (found == persistent_state_->windows.end()) return;

    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    auto& state = found->second;
    const workstation::LogicalRect previous_bounds = state.bounds;
    const workstation::LogicalRect next_bounds{
        (position.x - work_position_.x) / ui_scale_,
        (position.y - work_position_.y) / ui_scale_,
        size.x / ui_scale_,
        size.y / ui_scale_};
    const bool bounds_changed = Different(state.bounds, next_bounds);
    if (bounds_changed) {
        state.bounds = next_bounds;
        dirty_ = true;
    }

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (bounds_changed) {
            dragged_windows_.insert(state.id);
            if (snap_enabled_) {
                if (IsResizeCursor(ImGui::GetMouseCursor())) {
                    const auto minimum = minimum_sizes_.find(state.id);
                    SnapResizedBounds(
                        state.bounds, previous_bounds,
                        minimum == minimum_sizes_.end()
                            ? ImVec2(0.0f, 0.0f)
                            : minimum->second);
                    ImGui::SetWindowPos(
                        {work_position_.x + state.bounds.x * ui_scale_,
                         work_position_.y + state.bounds.y * ui_scale_},
                        ImGuiCond_Always);
                    ImGui::SetWindowSize(
                        {state.bounds.width * ui_scale_,
                         state.bounds.height * ui_scale_},
                        ImGuiCond_Always);
                } else {
                    const ImVec2 snapped = SnapPosition(
                        {state.bounds.x, state.bounds.y},
                        {state.bounds.width, state.bounds.height});
                    state.bounds.x = snapped.x;
                    state.bounds.y = snapped.y;
                    ImGui::SetWindowPos(
                        {work_position_.x + state.bounds.x * ui_scale_,
                         work_position_.y + state.bounds.y * ui_scale_},
                        ImGuiCond_Always);
                }
            }
        }
        return;
    }
    if (dragged_windows_.erase(state.id) == 0U) return;

    if (snap_enabled_) {
        const ImVec2 snapped = SnapPosition(
            {state.bounds.x, state.bounds.y},
            {state.bounds.width, state.bounds.height});
        state.bounds.x = snapped.x;
        state.bounds.y = snapped.y;
    }
    if (NormalizeBounds(state.bounds)) dirty_ = true;
    pending_geometry_.insert(state.id);
    dirty_ = true;
}

void Workspace::SnapResizedBounds(workstation::LogicalRect& bounds,
                                  const workstation::LogicalRect& previous,
                                  ImVec2 minimum_size) const {
    const float available_width = std::max(1.0f, work_size_.x / ui_scale_);
    const float available_height = std::max(1.0f, work_size_.y / ui_scale_);
    const float step = static_cast<float>(snap_pixels_);
    const auto snap_size = [step](float value, float minimum, float maximum) {
        const float clamped = std::clamp(value, minimum, maximum);
        const float lower = minimum +
                            std::floor((clamped - minimum) / step) * step;
        const float upper = std::min(maximum, lower + step);
        return std::fabs(clamped - upper) < std::fabs(clamped - lower)
                   ? upper
                   : lower;
    };
    const bool resized_from_left = std::fabs(bounds.x - previous.x) > 0.25f;
    const bool resized_from_top = std::fabs(bounds.y - previous.y) > 0.25f;
    const float right = previous.x + previous.width;
    const float bottom = previous.y + previous.height;
    bounds.width = snap_size(bounds.width, minimum_size.x,
                             resized_from_left ? right
                                               : available_width - bounds.x);
    bounds.height = snap_size(bounds.height, minimum_size.y,
                              resized_from_top ? bottom
                                               : available_height - bounds.y);
    if (resized_from_left) bounds.x = right - bounds.width;
    if (resized_from_top) bounds.y = bottom - bounds.height;
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

void Workspace::ReturnAllWindowsToWorkspace() {
    if (persistent_state_ == nullptr) return;
    for (auto& [id, window] : persistent_state_->windows) {
        if (NormalizeBounds(window.bounds)) {
            pending_geometry_.insert(id);
            dirty_ = true;
        }
    }
}

ImVec2 Workspace::SnapPosition(ImVec2 position, ImVec2 window_size) const {
    const float step = static_cast<float>(snap_pixels_);
    const auto snap_axis = [step](float value, float canvas_length, float size) {
        const float edge = std::max(0.0f, canvas_length - size);
        const float clamped = std::clamp(value, 0.0f, edge);
        const float grid = std::clamp(
            std::round(clamped / step) * step, 0.0f, edge);
        return std::fabs(edge - clamped) < std::fabs(grid - clamped)
                   ? edge
                   : grid;
    };
    return {snap_axis(position.x, work_size_.x / ui_scale_, window_size.x),
            snap_axis(position.y, work_size_.y / ui_scale_, window_size.y)};
}

}  // namespace tradebox::ui
