#pragma once

#include "tradebox/core/interfaces.h"
#include "tradebox/persistence/database.h"

namespace tradebox::persistence {

class DatabaseEventJournal final : public core::IEventJournal {
public:
    explicit DatabaseEventJournal(Database& database);

    std::expected<void, core::CoreError> Append(
        const core::BrokerEvent& event) override;

private:
    Database& database_;
};

}  // namespace tradebox::persistence
