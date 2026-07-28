#pragma once

#include "tradebox/core/types.h"

#include <chrono>
#include <expected>

namespace tradebox::core {

class IClock {
public:
    virtual ~IClock() = default;
    virtual std::chrono::system_clock::time_point Now() const = 0;
};

class IEventJournal {
public:
    virtual ~IEventJournal() = default;
    virtual std::expected<void, CoreError> Append(
        const BrokerEvent& event) = 0;
};

class ITradingCore {
public:
    virtual ~ITradingCore() = default;

    virtual CoreSnapshot Snapshot() const = 0;
    virtual std::expected<CommandReceipt, CoreError> Submit(
        Command command) = 0;
    virtual std::expected<void, CoreError> Ingest(BrokerEvent event) = 0;
};

}  // namespace tradebox::core
