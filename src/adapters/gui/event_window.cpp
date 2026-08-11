#include "event_window.h"

#include "tradebox/workstation/event_window.h"

#include "imgui.h"

#include <ctime>
#include <format>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace tradebox::gui {
namespace {

constexpr std::size_t kMaximumEvents = 1'000;

const char* SeverityLabel(OperationalSeverity severity) {
    switch (severity) {
        case OperationalSeverity::Informational: return "Info";
        case OperationalSeverity::Warning: return "Warning";
        case OperationalSeverity::Critical: return "Critical";
    }
    return "Unknown";
}

const char* ComponentLabel(OperationalComponent component) {
    switch (component) {
        case OperationalComponent::None: return "-";
        case OperationalComponent::Account: return "Account";
        case OperationalComponent::AccountStream: return "Account stream";
        case OperationalComponent::MarketDataStream: return "Market data";
        case OperationalComponent::Persistence: return "Persistence";
    }
    return "Unknown";
}

ImVec4 SeverityColor(OperationalSeverity severity, bool order_rejected) {
    if (order_rejected || severity == OperationalSeverity::Critical)
        return {.95f, .32f, .32f, 1.0f};
    if (severity == OperationalSeverity::Warning)
        return {.98f, .70f, .20f, 1.0f};
    return {.85f, .85f, .85f, 1.0f};
}

std::string Timestamp(std::int64_t milliseconds) {
    if (milliseconds <= 0) return "--";
    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1'000);
    std::tm local{};
    localtime_s(&local, &seconds);
    std::ostringstream output;
    output << std::put_time(&local, "%H:%M:%S") << '.' << std::setw(3)
           << std::setfill('0') << milliseconds % 1'000;
    return output.str();
}

bool OrderRejected(const UiEvent& event) {
    return event.type == UiEventType::OrderCommandCompleted &&
           !event.command_result.AcceptedByBroker();
}

std::string Message(const UiEvent& event) {
    if (event.type != UiEventType::OrderCommandCompleted)
        return event.message;
    return std::string(event.command_result.AcceptedByBroker()
                           ? "Order accepted: "
                           : "Order rejected: ") +
           event.command_result.message;
}

bool IsProblem(const UiEvent& event) {
    return OrderRejected(event) ||
           event.operational_severity != OperationalSeverity::Informational;
}

std::string ClipboardLine(const UiEvent& event) {
    return std::format("{}\t{}\t{}\t{}\t{}\n", Timestamp(event.received_at_ms),
                       SeverityLabel(event.operational_severity),
                       ComponentLabel(event.operational_component), event.symbol,
                       Message(event));
}

}  // namespace

void EventWindowRenderer::Append(std::vector<UiEvent> events) {
    for (UiEvent& event : events) {
        events_.push_front(std::move(event));
        if (events_.size() > kMaximumEvents) events_.pop_back();
    }
}

void EventWindowRenderer::Draw(ui::Workspace& workspace,
                               workstation::WorkspaceState& state) {
    if (requested_open_) {
        static_cast<void>(workstation::CreateEventWindow(state));
        workspace.MarkDirty();
        requested_open_ = false;
    }
    const auto found = state.windows.find(
        std::string(workstation::kEventWindowId));
    if (found == state.windows.end() || !found->second.open) return;

    ui::WorkspaceWindow window{
        .title = "Events",
        .id = std::string(workstation::kEventWindowId),
        .default_offset = {520, 600},
        .default_size = {900, 300},
        .open = true,
    };
    if (!workspace.BeginWindow(window)) {
        workspace.EndWindow(window);
        return;
    }

    if (ImGui::Button("Clear")) events_.clear();
    ImGui::SameLine();
    ImGui::Checkbox("Warnings and critical only", &problems_only_);
    ImGui::SameLine();
    if (ImGui::Button("Copy visible")) {
        std::string clipboard;
        for (const UiEvent& event : events_)
            if (!problems_only_ || IsProblem(event))
                clipboard += ClipboardLine(event);
        ImGui::SetClipboardText(clipboard.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Newest first · %zu retained", events_.size());

    constexpr ImGuiTableFlags table_flags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("event_log", 5, table_flags, {0, 0})) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed,
                                96.0f);
        ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed,
                                76.0f);
        ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthFixed,
                                112.0f);
        ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed,
                                72.0f);
        ImGui::TableSetupColumn("Message");
        ImGui::TableHeadersRow();
        std::vector<const UiEvent*> visible_events;
        visible_events.reserve(events_.size());
        for (const UiEvent& event : events_)
            if (!problems_only_ || IsProblem(event))
                visible_events.push_back(&event);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible_events.size()));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd;
                 ++index) {
            const UiEvent& event = *visible_events[static_cast<std::size_t>(index)];
            const bool order_rejected = OrderRejected(event);
            const ImVec4 color = SeverityColor(
                event.operational_severity, order_rejected);
            const std::string message = Message(event);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Timestamp(event.received_at_ms).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(color, "%s",
                               order_rejected ? "Critical"
                                              : SeverityLabel(event.operational_severity));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ComponentLabel(event.operational_component));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(event.symbol.empty() ? "-" : event.symbol.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(color, "%s", message.c_str());
            }
        }
        ImGui::EndTable();
    }
    workspace.EndWindow(window);
}

}  // namespace tradebox::gui
