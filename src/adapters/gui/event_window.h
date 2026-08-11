#pragma once
#include "tradebox/ui/model.h"
#include "tradebox/ui/workspace.h"

#include <deque>

namespace tradebox::gui {

class EventWindowRenderer final {
public:
 void Open() { requested_open_ = true; }
 void Append(std::vector<UiEvent> events);
 void Draw(ui::Workspace&, workstation::WorkspaceState&);

private:
 bool requested_open_ = false;
 bool problems_only_ = false;
 std::deque<UiEvent> events_;
};

}  // namespace tradebox::gui
