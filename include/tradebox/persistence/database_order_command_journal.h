#pragma once

#include "tradebox/core/order_command.h"
#include "tradebox/persistence/database.h"

namespace tradebox::persistence {

class DatabaseOrderCommandJournal final
    : public core::IOrderCommandJournal {
public:
    explicit DatabaseOrderCommandJournal(Database& database)
        : database_(database) {}

    std::expected<core::ReservationResult, std::string> Reserve(
        const core::OrderCommandRecord& record,
        const core::NativeOrderCommand& command) override {
        return database_.ReserveOrderCommand(record, command);
    }

    std::expected<void, std::string> Complete(
        const core::OrderCommandResult& result) override {
        return database_.CompleteOrderCommand(result);
    }

    std::expected<void, std::string> MarkDispatchStarted(
        const std::string& request_id) override {
        return database_.MarkOrderCommandDispatchStarted(request_id);
    }

    std::expected<std::vector<core::RecoverableOrderCommand>, std::string>
    Recoverable() override {
        return database_.LoadRecoverableOrderCommands();
    }

    std::expected<void, std::string> ResolveRecovery(
        const core::OrderCommandResult& result) override {
        return database_.ResolveOrderCommandRecovery(result);
    }

    std::expected<core::OrderCommandLookup, std::string>
    Lookup(const std::string& request_id) override {
        return database_.LookupOrderCommand(request_id);
    }

private:
    Database& database_;
};

}  // namespace tradebox::persistence
