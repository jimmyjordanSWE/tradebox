#include "tradebox/workstation/state.h"
#include "tradebox/workstation/stable_id.h"

namespace tradebox::workstation {

WorkstationState WorkstationState::Defaults() {
    WorkstationState state;
    state.profile.id = NewStableId("profile");
    // The initial workstation is deliberately a blank shell. Window/document
    // definitions are introduced by the rebuilt UI rather than by defaults.
    return state;
}

}  // namespace tradebox::workstation
