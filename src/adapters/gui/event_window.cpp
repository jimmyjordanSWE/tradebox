#include "event_window.h"
#include "tradebox/workstation/event_window.h"
#include "imgui.h"
#include <chrono>
#include <format>
namespace tradebox::gui {
void EventWindowRenderer::Append(std::vector<UiEvent> events) { for (auto& e: events) { events_.push_front(std::move(e)); if(events_.size()>1000) events_.pop_back(); } }
void EventWindowRenderer::Draw(ui::Workspace& workspace, workstation::WorkspaceState& state) {
 if (requested_open_) { static_cast<void>(workstation::CreateEventWindow(state)); workspace.MarkDirty(); requested_open_=false; }
 const auto it=state.windows.find(std::string(workstation::kEventWindowId)); if(it==state.windows.end()||!it->second.open) return;
 ui::WorkspaceWindow window{.title="Events",.id=std::string(workstation::kEventWindowId),.default_offset={520,600},.default_size={760,260},.open=true};
 if(!workspace.BeginWindow(window)){workspace.EndWindow(window);return;}
 if(ImGui::Button("Clear")) events_.clear(); ImGui::SameLine(); ImGui::TextDisabled("Newest first");
 if(ImGui::BeginChild("event_log",{},ImGuiChildFlags_Borders)) for(const auto& e:events_) {
   const bool order=e.type==UiEventType::OrderCommandCompleted;
   const bool bad=order && !e.command_result.AcceptedByBroker();
   const std::string message=order ? (e.command_result.AcceptedByBroker()?"Order accepted: ":"Order rejected: ")+e.command_result.message : e.message;
   ImGui::TextColored(bad?ImVec4{.95f,.32f,.32f,1}:ImVec4{.85f,.85f,.85f,1}, "%lld  %s", static_cast<long long>(e.received_at_ms), message.c_str());
 } ImGui::EndChild(); workspace.EndWindow(window);
}
}
