#pragma once

#include "tradebox/core/order_request.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>

namespace tradebox::core {

struct OrderCommandContext {
    std::string request_id;
    std::string source;
    std::string account_id;
    AccountEnvironment environment = AccountEnvironment::Paper;
    ConnectionGeneration generation;
    bool live_trading_confirmed = false;
};

struct PlaceOrderCommand {
    OrderCommandContext context;
    NativeOrderRequest order;
};

struct CancelOrderCommand {
    OrderCommandContext context;
    std::string order_id;
};

struct ReplaceOrderCommand {
    OrderCommandContext context;
    std::string order_id;
    ReplaceOrderRequest replacement;
};

using NativeOrderCommand =
    std::variant<PlaceOrderCommand, CancelOrderCommand, ReplaceOrderCommand>;

enum class OrderCommandOutcome {
    ValidationRejected,
    SafetyRejected,
    BrokerAccepted,
    BrokerRejected,
    Indeterminate,
    Duplicate,
    ServiceStopped,
};

struct OrderCommandResult {
    std::string request_id;
    OrderCommandOutcome outcome = OrderCommandOutcome::Indeterminate;
    std::string broker_order_id;
    std::uint32_t http_status = 0;
    std::string message;
    std::string raw_response;

    [[nodiscard]] bool AcceptedByBroker() const {
        return outcome == OrderCommandOutcome::BrokerAccepted;
    }
};

enum class OrderCommandKind { Place, Cancel, Replace };

struct OrderCommandRecord {
    std::string request_id;
    std::string source;
    OrderCommandKind kind = OrderCommandKind::Place;
    std::string account_id;
    AccountEnvironment environment = AccountEnvironment::Paper;
    ConnectionGeneration generation;
    std::string client_order_id;
    std::int64_t created_at_ms = 0;
};

enum class CommandReservation { Reserved, Duplicate };

struct ReservationResult {
    CommandReservation reservation = CommandReservation::Reserved;
    std::optional<OrderCommandResult> existing_result;
};

struct OrderCommandLookup {
    bool exists = false;
    std::optional<OrderCommandResult> terminal_result;
};

class IOrderCommandJournal {
public:
    virtual ~IOrderCommandJournal() = default;

    virtual std::expected<ReservationResult, std::string> Reserve(
        const OrderCommandRecord& record,
        const NativeOrderCommand& command) = 0;
    virtual std::expected<void, std::string> Complete(
        const OrderCommandResult& result) = 0;
    virtual std::expected<OrderCommandLookup, std::string>
    Lookup(const std::string& request_id) = 0;
};

}  // namespace tradebox::core
