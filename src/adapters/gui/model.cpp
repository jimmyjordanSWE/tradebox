#include "tradebox/ui/model.h"
#include "tradebox/core/order_projection.h"

#include <algorithm>

namespace {
using namespace tradebox::core;

std::optional<Decimal> Parse(const std::string& value) {
    if (value.empty()) return std::nullopt;
    const auto parsed = Decimal::Parse(value);
    if (!parsed) return std::nullopt;
    return *parsed;
}
}

const char* OperationalComponentLabel(OperationalComponent component) {
    switch (component) {
    case OperationalComponent::None: return "none";
    case OperationalComponent::Account: return "account";
    case OperationalComponent::AccountStream: return "account_stream";
    case OperationalComponent::MarketDataStream:
        return "market_data_stream";
    case OperationalComponent::Persistence:
        return "persistence";
    }
    return "none";
}

const char* OperationalStateLabel(OperationalState state) {
    switch (state) {
    case OperationalState::None: return "none";
    case OperationalState::Connecting: return "connecting";
    case OperationalState::Reconnecting: return "reconnecting";
    case OperationalState::Authenticated: return "authenticated";
    case OperationalState::Subscribed: return "subscribed";
    case OperationalState::Degraded: return "degraded";
    case OperationalState::Disconnected: return "disconnected";
    case OperationalState::Failed: return "failed";
    }
    return "none";
}

const char* OperationalReasonLabel(OperationalReason reason) {
    switch (reason) {
    case OperationalReason::None: return "none";
    case OperationalReason::TransportFailure:
        return "transport_failure";
    case OperationalReason::UpgradeFailure:
        return "upgrade_failure";
    case OperationalReason::AuthenticationFailure:
        return "authentication_failure";
    case OperationalReason::SubscriptionMismatch:
        return "subscription_mismatch";
    case OperationalReason::UnexpectedDisconnect:
        return "unexpected_disconnect";
    case OperationalReason::SilenceTimeout:
        return "silence_timeout";
    case OperationalReason::QueueOverload:
        return "queue_overload";
    case OperationalReason::PersistenceFailure:
        return "persistence_failure";
    case OperationalReason::SecurityPolicyFailure:
        return "security_policy_failure";
    case OperationalReason::PayloadLimitExceeded:
        return "payload_limit_exceeded";
    }
    return "none";
}

const char* OperationalSeverityLabel(OperationalSeverity severity) {
    switch (severity) {
    case OperationalSeverity::Informational: return "informational";
    case OperationalSeverity::Warning: return "warning";
    case OperationalSeverity::Critical: return "critical";
    }
    return "informational";
}

std::vector<UiValidationMessage> ValidateOrderEntry(
    const OrderEntryDraft& draft) {
    std::vector<UiValidationMessage> errors;
    const auto request = BuildNativeOrderRequest(draft, errors);
    if (!request) return errors;
    for (const auto& error : tradebox::core::ValidateOrder(*request))
        errors.push_back({error.field, error.message});
    return errors;
}

std::optional<tradebox::core::NativeOrderRequest>
BuildNativeOrderRequest(const OrderEntryDraft& draft,
                        std::vector<UiValidationMessage>& errors) {
    NativeOrderRequest request;
    request.asset_class = AssetClass::Equity;
    request.symbol = draft.symbol;
    request.side = draft.side == "Sell" ? OrderSide::Sell : OrderSide::Buy;
    request.type = draft.type == "Limit"
                       ? OrderType::Limit
                       : draft.type == "Stop"
                             ? OrderType::Stop
                             : draft.type == "Stop-limit"
                                   ? OrderType::StopLimit
                                   : OrderType::Market;
    request.time_in_force = draft.time_in_force == "Gtc"
                                ? TimeInForce::Gtc
                                : TimeInForce::Day;
    request.extended_hours = draft.extended_hours;

    const auto amount = Parse(draft.amount);
    if (!amount && !draft.amount.empty()) {
        errors.push_back({"amount", draft.amount.empty()
                                      ? "quantity or notional is required"
                                      : "must be a valid decimal"});
    } else if (amount && draft.amount_is_notional) {
        request.notional = *amount;
    } else if (amount) {
        request.qty = *amount;
    }

    const auto parse_price = [&errors](const std::string& value,
                                       const char* field)
        -> std::optional<Decimal> {
        if (value.empty()) return std::nullopt;
        const auto parsed = Decimal::Parse(value);
        if (!parsed)
            errors.push_back({field, "must be a valid decimal"});
        return *parsed;
    };
    request.limit_price = parse_price(draft.limit_price, "limit_price");
    request.stop_price = parse_price(draft.stop_price, "stop_price");
    if (!errors.empty()) return std::nullopt;
    return request;
}

std::string UiOrderStateLabel(UiOrderState state) {
    switch (state) {
    case UiOrderState::Pending: return "pending";
    case UiOrderState::Accepted: return "accepted";
    case UiOrderState::Rejected: return "rejected";
    case UiOrderState::Canceled: return "canceled";
    case UiOrderState::Filled: return "filled";
    case UiOrderState::Indeterminate: return "indeterminate";
    case UiOrderState::Stale: return "stale";
    case UiOrderState::Reconciling: return "reconciling";
    }
    return "indeterminate";
}

UiOrderState UiOrderStateFromCore(const OrderState& order,
                                  const CoreSnapshot& snapshot,
                                  bool command_indeterminate) {
    if (command_indeterminate) return UiOrderState::Indeterminate;
    static_cast<void>(snapshot);
    switch (ProjectOrder(order).lifecycle) {
    case OrderLifecycleState::Pending: return UiOrderState::Pending;
    case OrderLifecycleState::Accepted: return UiOrderState::Accepted;
    case OrderLifecycleState::Rejected: return UiOrderState::Rejected;
    case OrderLifecycleState::Canceled: return UiOrderState::Canceled;
    case OrderLifecycleState::Filled: return UiOrderState::Filled;
    case OrderLifecycleState::Unknown: return UiOrderState::Indeterminate;
    }
    return UiOrderState::Indeterminate;
}
