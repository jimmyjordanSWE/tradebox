#include "tradebox/ui/model.h"

#include <algorithm>
#include <cctype>

namespace {
using namespace tradebox::core;

bool Empty(const std::string& value) { return value.empty(); }

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
    std::vector<UiValidationMessage> result;
    if (draft.symbol.empty()) result.push_back({"symbol", "Symbol is required"});
    if (draft.amount.empty()) result.push_back({"amount", "Quantity or notional is required"});
    else if (!Parse(draft.amount) || *Parse(draft.amount) <= Decimal::Zero())
        result.push_back({"amount", "Amount must be a positive decimal"});
    if (draft.type == "Limit" || draft.type == "Stop-limit") {
        if (!Parse(draft.limit_price) || *Parse(draft.limit_price) <= Decimal::Zero())
            result.push_back({"limit_price", "A positive limit price is required"});
    }
    if (draft.type == "Stop" || draft.type == "Stop-limit") {
        if (!Parse(draft.stop_price) || *Parse(draft.stop_price) <= Decimal::Zero())
            result.push_back({"stop_price", "A positive stop price is required"});
    }
    if (draft.amount_is_notional && draft.type != "Market")
        result.push_back({"amount", "Notional orders must use Market"});
    if (draft.extended_hours && draft.time_in_force != "Day")
        result.push_back({"time_in_force", "Extended hours requires Day"});
    return result;
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
    if (snapshot.safety_status == SafetyStatus::Reconciling)
        return UiOrderState::Reconciling;
    if (snapshot.safety_status == SafetyStatus::Stale)
        return UiOrderState::Stale;
    std::string status = order.status;
    std::transform(status.begin(), status.end(), status.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (status == "filled") return UiOrderState::Filled;
    if (status == "canceled" || status == "cancelled") return UiOrderState::Canceled;
    if (status == "rejected" || status == "expired") return UiOrderState::Rejected;
    if (status == "new" || status == "accepted" || status == "partially_filled")
        return UiOrderState::Accepted;
    return UiOrderState::Indeterminate;
}
