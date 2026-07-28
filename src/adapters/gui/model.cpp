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
