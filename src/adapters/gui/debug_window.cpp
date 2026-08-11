#include "debug_window.h"

#include "imgui.h"

#include <iomanip>
#include <sstream>

namespace tradebox::gui {
namespace {

constexpr std::string_view kDebugWindowId = "debug.window";

std::string Fixed(double value, int decimals = 2) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(decimals) << value;
    return output.str();
}

void Row(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(value.c_str());
}

}  // namespace

void DebugWindowRenderer::Draw(
    ui::Workspace& workspace, workstation::WorkspaceState& state,
    const DebugSnapshot& snapshot) {
    const bool open_requested = requested_open_;
    if (open_requested) {
        if (const auto persisted =
                state.windows.find(std::string(kDebugWindowId));
            persisted != state.windows.end()) {
            const bool was_closed = !persisted->second.open;
            persisted->second.open = true;
            if (was_closed) workspace.MarkDirty();
        }
        requested_open_ = false;
    }
    const auto persisted = state.windows.find(std::string(kDebugWindowId));
    if (!open_requested &&
        (persisted == state.windows.end() || !persisted->second.open))
        return;

    ui::WorkspaceWindow window{
        .title = "Debug",
        .id = std::string(kDebugWindowId),
        .default_offset = {96.0f, 96.0f},
        .default_size = {620.0f, 620.0f},
        .open = true,
    };
    workspace.ConstrainNextWindowSize({420.0f, 320.0f});
    if (!workspace.BeginWindow(window)) {
        workspace.EndWindow(window);
        return;
    }

    if (ImGui::BeginTable("##debug_values", 2,
                          ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed,
                                220.0f);
        ImGui::TableSetupColumn("Value");
        ImGui::TableHeadersRow();
        Row("Measured FPS", Fixed(snapshot.frames_per_second));
        Row("Measured frame time", Fixed(snapshot.frame_time_ms) + " ms");
        Row("VSync requested", snapshot.vsync_requested ? "On" : "Off");
        Row("Frame-latency waitable", snapshot.frame_latency_waitable
                                           ? "Enabled (max 1 frame)"
                                           : "Unavailable");
        Row("Configured max FPS",
            snapshot.maximum_frame_rate == 0
                ? "Unlimited"
                : std::to_string(snapshot.maximum_frame_rate));
        if (snapshot.rest_health) {
            const auto& health = *snapshot.rest_health;
            Row("Historical work", std::to_string(health.historical_work_queued) +
                                       " queued, " +
                                       std::to_string(health.historical_work_in_flight) +
                                       " active");
            Row("Historical work totals",
                std::to_string(health.historical_work_completed) +
                    " completed, " +
                    std::to_string(health.historical_work_coalesced) +
                    " coalesced, " +
                    std::to_string(health.historical_work_rejected) +
                    " rejected");
            Row("REST market-data budget",
                std::to_string(health.market_data.remaining) + " remaining / " +
                    std::to_string(health.market_data.limit));
        }
        Row("Window pixels", std::to_string(snapshot.window_width) + " x " +
                                  std::to_string(snapshot.window_height));
        Row("Display mode", std::to_string(snapshot.display_width) + " x " +
                                  std::to_string(snapshot.display_height) +
                                  " @ " + Fixed(snapshot.display_refresh_rate) +
                                  " Hz");
        Row("DXGI adapter", snapshot.adapter_name);
        Row("Adapter vendor/device",
            "0x" + [&snapshot] {
                std::ostringstream value;
                value << std::hex << snapshot.adapter_vendor_id << " / 0x"
                      << snapshot.adapter_device_id;
                return value.str();
            }());
        Row("Dedicated video memory",
            Fixed(static_cast<double>(snapshot.dedicated_video_memory) /
                      (1024.0 * 1024.0 * 1024.0)) +
                " GiB");
        Row("Shared system memory",
            Fixed(static_cast<double>(snapshot.shared_system_memory) /
                      (1024.0 * 1024.0 * 1024.0)) +
                " GiB");
        Row("D3D feature level", snapshot.feature_level);
        Row("SDL runtime", snapshot.sdl_version);
        Row("Platform", snapshot.platform);
        Row("Logical CPU cores", std::to_string(snapshot.logical_cpu_cores));
        Row("System memory", std::to_string(snapshot.system_memory_mb) +
                                  " MiB");
        Row("Compiler", snapshot.compiler);
        Row("STL", snapshot.stl);
        Row("C++ mode", snapshot.cxx_standard);
        Row("Steady clock", snapshot.steady_clock);
        ImGui::EndTable();
    }
    workspace.EndWindow(window);
}

}  // namespace tradebox::gui
