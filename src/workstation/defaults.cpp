#include "tradebox/workstation/state.h"

#include <array>
#include <random>
#include <sstream>

namespace tradebox::workstation {
namespace {

std::string NewId() {
    std::random_device device;
    std::mt19937_64 random(device());
    const auto next = [&random] { return random(); };
    std::ostringstream output;
    output << std::hex;
    output << (next() & 0xffffffffULL) << '-' << (next() & 0xffffULL) << '-'
           << ((next() & 0x0fffULL) | 0x4000ULL) << '-'
           << ((next() & 0x3fffULL) | 0x8000ULL) << '-' << (next() & 0xffffffffffffULL);
    return output.str();
}

}  // namespace

WorkstationState WorkstationState::Defaults() {
    WorkstationState state;
    state.profile.id = NewId();
    // The initial workstation is deliberately a blank shell. Window/document
    // definitions are introduced by the rebuilt UI rather than by defaults.
    return state;
}

}  // namespace tradebox::workstation

