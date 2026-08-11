#pragma once

#include "tradebox/core/rest_health.h"
#include "tradebox/ui/workspace.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tradebox::gui {

struct DebugSnapshot {
    double frames_per_second = 0.0;
    double frame_time_ms = 0.0;
    bool vsync_requested = true;
    bool frame_latency_waitable = false;
    int maximum_frame_rate = 0;
    int window_width = 0;
    int window_height = 0;
    int display_width = 0;
    int display_height = 0;
    float display_refresh_rate = 0.0f;
    std::string adapter_name;
    std::uint32_t adapter_vendor_id = 0;
    std::uint32_t adapter_device_id = 0;
    std::uint64_t dedicated_video_memory = 0;
    std::uint64_t shared_system_memory = 0;
    std::string feature_level;
    std::string sdl_version;
    std::string platform;
    int logical_cpu_cores = 0;
    int system_memory_mb = 0;
    std::string compiler;
    std::string stl;
    std::string cxx_standard;
    std::string steady_clock;
    std::optional<core::RestTransportHealth> rest_health;
};

class DebugWindowRenderer final {
public:
    void Open() { requested_open_ = true; }
    void Draw(ui::Workspace& workspace, workstation::WorkspaceState& state,
              const DebugSnapshot& snapshot);

private:
    bool requested_open_ = false;
};

}  // namespace tradebox::gui
