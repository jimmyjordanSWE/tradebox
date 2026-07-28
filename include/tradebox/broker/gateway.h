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
};

}  // namespace tradebox::broker
