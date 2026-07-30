#pragma once

#include "tradebox/core/order_command.h"
#include "tradebox/core/types.h"

#include <expected>
#include <functional>

namespace tradebox::broker {

using BrokerEventSink =
    std::function<void(tradebox::core::BrokerEvent)>;

class IBrokerGateway {
public:
    virtual ~IBrokerGateway() = default;

    virtual std::expected<void, tradebox::core::CoreError> Start(
        BrokerEventSink sink) = 0;
    virtual void Stop() = 0;
};

enum class BrokerCommandDisposition {
    Accepted,
    PartiallyAccepted,
    Rejected,
    Indeterminate,
};

struct BrokerCommandResult {
    BrokerCommandDisposition disposition =
        BrokerCommandDisposition::Indeterminate;
    std::uint32_t http_status = 0;
    std::string broker_order_id;
    std::string message;
    std::string raw_response;
    std::vector<tradebox::core::CommandItemResult> items;
    bool reconciliation_required = false;
};

class IOrderGateway {
public:
    virtual ~IOrderGateway() = default;

    virtual BrokerCommandResult PlaceOrder(
        const tradebox::core::NativeOrderRequest& request) = 0;
    virtual BrokerCommandResult CancelOrder(
        const std::string& order_id) = 0;
    virtual BrokerCommandResult ReplaceOrder(
        const std::string& order_id,
        const tradebox::core::ReplaceOrderRequest& request) = 0;
    virtual BrokerCommandResult ClosePosition(
        const std::string& symbol_or_asset_id,
        const std::optional<tradebox::core::Decimal>& qty,
        const std::optional<tradebox::core::Decimal>& percentage) = 0;
    virtual BrokerCommandResult CloseAllPositions(
        bool cancel_open_orders) = 0;
    virtual BrokerCommandResult CancelAllOrders() = 0;
};

}  // namespace tradebox::broker
