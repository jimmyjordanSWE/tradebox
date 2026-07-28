#include "tradebox/core/system_clock.h"

namespace tradebox::core {

std::chrono::system_clock::time_point SystemClock::Now() const {
    return std::chrono::system_clock::now();
}

}  // namespace tradebox::core
