#include "tradebox/broker/alpaca_order_codec.h"

#include <nlohmann/json.hpp>

#include <string_view>

namespace tradebox::broker::alpaca {
namespace {

using json = nlohmann::json;

std::string_view Value(core::OrderSide value) {
    return value == core::OrderSide::Buy ? "buy" : "sell";
}

std::string_view Value(core::OrderType value) {
    switch (value) {
        case core::OrderType::Market:
            return "market";
        case core::OrderType::Limit:
            return "limit";
        case core::OrderType::Stop:
            return "stop";
        case core::OrderType::StopLimit:
            return "stop_limit";
        case core::OrderType::TrailingStop:
            return "trailing_stop";
    }
    return {};
}

std::string_view Value(core::TimeInForce value) {
    switch (value) {
        case core::TimeInForce::Day:
            return "day";
        case core::TimeInForce::Gtc:
            return "gtc";
        case core::TimeInForce::Opg:
            return "opg";
        case core::TimeInForce::Cls:
            return "cls";
        case core::TimeInForce::Ioc:
            return "ioc";
        case core::TimeInForce::Fok:
            return "fok";
    }
    return {};
}

std::string_view Value(core::OrderClass value) {
    switch (value) {
        case core::OrderClass::Simple:
            return "simple";
        case core::OrderClass::Bracket:
            return "bracket";
        case core::OrderClass::Oco:
            return "oco";
        case core::OrderClass::Oto:
            return "oto";
        case core::OrderClass::Mleg:
            return "mleg";
    }
    return {};
}

std::string_view Value(core::PositionIntent value) {
    switch (value) {
        case core::PositionIntent::BuyToOpen:
            return "buy_to_open";
        case core::PositionIntent::BuyToClose:
            return "buy_to_close";
        case core::PositionIntent::SellToOpen:
            return "sell_to_open";
        case core::PositionIntent::SellToClose:
            return "sell_to_close";
    }
    return {};
}

void DecimalField(json& value, const char* name,
                  const std::optional<core::Decimal>& decimal) {
    if (decimal) value[name] = decimal->ToString();
}

}  // namespace

std::expected<std::string, std::string> SerializeOrder(
    const core::NativeOrderRequest& request) {
    const auto errors = core::ValidateOrder(request);
    if (!errors.empty())
        return std::unexpected(errors.front().field + ": " +
                               errors.front().message);

    json value;
    if (!request.symbol.empty()) value["symbol"] = request.symbol;
    DecimalField(value, "qty", request.qty);
    DecimalField(value, "notional", request.notional);
    if (request.side) value["side"] = Value(*request.side);
    value["type"] = Value(request.type);
    value["time_in_force"] = Value(request.time_in_force);
    value["order_class"] = Value(request.order_class);
    if (request.position_intent)
        value["position_intent"] = Value(*request.position_intent);
    DecimalField(value, "limit_price", request.limit_price);
    DecimalField(value, "stop_price", request.stop_price);
    DecimalField(value, "trail_price", request.trail_price);
    DecimalField(value, "trail_percent", request.trail_percent);
    value["extended_hours"] = request.extended_hours;
    if (!request.client_order_id.empty())
        value["client_order_id"] = request.client_order_id;
    if (request.take_profit)
        value["take_profit"] = {
            {"limit_price",
             request.take_profit->limit_price.ToString()},
        };
    if (request.stop_loss) {
        json stop = {
            {"stop_price", request.stop_loss->stop_price.ToString()},
        };
        DecimalField(stop, "limit_price",
                     request.stop_loss->limit_price);
        value["stop_loss"] = std::move(stop);
    }
    if (!request.legs.empty()) {
        value["legs"] = json::array();
        for (const core::OrderLeg& leg : request.legs) {
            value["legs"].push_back({
                {"symbol", leg.symbol},
                {"ratio_qty", leg.ratio_qty.ToString()},
                {"side", Value(leg.side)},
                {"position_intent", Value(leg.position_intent)},
            });
        }
    }
    return value.dump();
}

std::expected<std::string, std::string> SerializeReplacement(
    const core::ReplaceOrderRequest& request) {
    json value;
    DecimalField(value, "qty", request.qty);
    DecimalField(value, "notional", request.notional);
    if (request.time_in_force)
        value["time_in_force"] = Value(*request.time_in_force);
    DecimalField(value, "limit_price", request.limit_price);
    DecimalField(value, "stop_price", request.stop_price);
    DecimalField(value, "trail", request.trail);
    if (request.client_order_id)
        value["client_order_id"] = *request.client_order_id;
    if (value.empty())
        return std::unexpected("replacement changes no fields");
    return value.dump();
}

}  // namespace tradebox::broker::alpaca
