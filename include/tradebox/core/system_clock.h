#pragma once

#include "tradebox/core/interfaces.h"

namespace tradebox::core {

class SystemClock final : public IClock {
public:
    std::chrono::system_clock::time_point Now() const override;
};

}  // namespace tradebox::core
