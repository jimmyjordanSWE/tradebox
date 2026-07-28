#include "tradebox/persistence/database_event_journal.h"

#include <chrono>
#include <string_view>
#include <utility>

namespace tradebox::persistence {
namespace {

std::string_view EventKindName(core::BrokerEventKind kind) {
    using enum core::BrokerEventKind;
    switch (kind) {
        case ConnectionAttemptStarted:
            return "connection_attempt_started";
        case Authorized:
            return "authorized";
        case TradeUpdatesAcknowledged:
            return "trade_updates_acknowledged";
        case Disconnected:
            return "disconnected";
        case AccountSnapshot:
            return "account_snapshot";
        case PositionsSnapshot:
            return "positions_snapshot";
        case OrdersSnapshot:
            return "orders_snapshot";
        case TradeUpdate:
            return "trade_update";
        case ReconciliationStarted:
            return "reconciliation_started";
        case ReconciliationCompleted:
            return "reconciliation_completed";
        case Failure:
            return "failure";
    }
    return "unknown";
}

}  // namespace

DatabaseEventJournal::DatabaseEventJournal(Database& database)
    : database_(database) {}

std::expected<void, core::CoreError> DatabaseEventJournal::Append(
    const core::BrokerEvent& event) {
    const auto received_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            event.received_at.time_since_epoch())
            .count();
    std::string error;
    if (!database_.AppendCoreEvent(
            event.source_event_id, std::string(EventKindName(event.kind)),
            event.generation.value, received_at_ms, event.raw_payload, error)) {
        return std::unexpected(core::CoreError{
            .code = core::CoreErrorCode::JournalFailure,
            .message = std::move(error),
        });
    }
    return {};
}

}  // namespace tradebox::persistence
