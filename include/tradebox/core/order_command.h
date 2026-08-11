#pragma once

#include "tradebox/core/order_request.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>
#include <vector>

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

struct CancelBracketOrderCommand {
    OrderCommandContext context;
    std::string parent_order_id;
};

struct ReplaceOrderCommand {
    OrderCommandContext context;
    std::string order_id;
    ReplaceOrderRequest replacement;
};

struct BracketLegAmendment {
    std::string order_id;
    ReplaceOrderRequest replacement;
};

struct AmendBracketOrderCommand {
    OrderCommandContext context;
    std::string parent_order_id;
    std::vector<BracketLegAmendment> amendments;
};

struct ClosePositionCommand {
    OrderCommandContext context;
    std::string symbol_or_asset_id;
    std::optional<Decimal> qty;
    std::optional<Decimal> percentage;
    bool cancel_open_orders = false;
};

struct CloseAllPositionsCommand {
    OrderCommandContext context;
    bool cancel_open_orders = false;
};

struct CancelAllOrdersCommand {
    OrderCommandContext context;
};

using NativeOrderCommand =
    std::variant<PlaceOrderCommand, CancelOrderCommand, CancelBracketOrderCommand,
                 ReplaceOrderCommand, AmendBracketOrderCommand,
                 ClosePositionCommand,
                 CloseAllPositionsCommand, CancelAllOrdersCommand>;

enum class OrderCommandOutcome {
    ValidationRejected,
    SafetyRejected,
    BrokerAccepted,
    BrokerRejected,
    Indeterminate,
    Duplicate,
    ServiceStopped,
    PartiallyAccepted,
    NotDispatched,
    RecoveryRejected,
};

enum class CommandRecoveryState {
    NotRequired,
    Pending,
    Resolved,
    Rejected,
    OperatorAttention,
};

struct CommandItemResult {
    std::string id;
    std::string symbol;
    std::uint32_t http_status = 0;
    bool accepted = false;
    std::string message;
    std::string raw_response;
};

struct OrderCommandResult {
    std::string request_id;
    OrderCommandOutcome outcome = OrderCommandOutcome::Indeterminate;
    std::string broker_order_id;
    std::uint32_t http_status = 0;
    std::string message;
    std::string raw_response;
    std::vector<CommandItemResult> items;
    bool reconciliation_required = false;
    CommandRecoveryState recovery_state =
        CommandRecoveryState::NotRequired;
    std::string recovery_message;

    [[nodiscard]] bool AcceptedByBroker() const {
        return outcome == OrderCommandOutcome::BrokerAccepted;
    }
};

enum class OrderCommandKind {
    Place,
    Cancel,
    Replace,
    AmendBracket,
    ClosePosition,
    CloseAllPositions,
    CancelAllOrders,
    CancelBracket,
};

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
    CommandRecoveryState recovery_state =
        CommandRecoveryState::NotRequired;
    std::string recovery_message;
};

struct RecoverableOrderCommand {
    OrderCommandRecord record;
    std::string target_order_id;
    std::string symbol_or_asset_id;
    std::optional<Decimal> qty;
    std::optional<Decimal> percentage;
    bool cancel_open_orders = false;
    bool dispatch_started = false;
    CommandRecoveryState recovery_state =
        CommandRecoveryState::Pending;
    std::string recovery_message;
};

class IOrderCommandJournal {
public:
    virtual ~IOrderCommandJournal() = default;

    virtual std::expected<ReservationResult, std::string> Reserve(
        const OrderCommandRecord& record,
        const NativeOrderCommand& command) = 0;
    virtual std::expected<void, std::string> Complete(
        const OrderCommandResult& result) = 0;
    virtual std::expected<void, std::string> MarkDispatchStarted(
        const std::string& request_id) = 0;
    virtual std::expected<std::vector<RecoverableOrderCommand>, std::string>
    Recoverable() = 0;
    virtual std::expected<void, std::string> ResolveRecovery(
        const OrderCommandResult& result) = 0;
    virtual std::expected<OrderCommandLookup, std::string>
    Lookup(const std::string& request_id) = 0;
};

}  // namespace tradebox::core
