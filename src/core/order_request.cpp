#include "tradebox/core/order_request.h"

#include <array>
#include <string_view>

namespace tradebox::core {
namespace {

void RequirePositive(const std::optional<Decimal>& value,
                     std::string_view field,
                     std::vector<OrderValidationError>& errors) {
    if (value && *value <= Decimal::Zero())
        errors.push_back({std::string(field), "must be greater than zero"});
}

bool IsProhibitedReplaceStatus(std::string_view status) {
    static constexpr std::array<std::string_view, 4> prohibited = {
        "accepted", "pending_new", "pending_cancel", "pending_replace"};
    for (std::string_view candidate : prohibited)
        if (candidate == status) return true;
    return false;
}

bool HasReplacementField(const ReplaceOrderRequest& request) {
    return request.qty || request.notional || request.time_in_force ||
           request.limit_price || request.stop_price || request.trail ||
           request.client_order_id;
}

}  // namespace

std::vector<OrderValidationError> ValidateOrder(
    const NativeOrderRequest& request) {
    std::vector<OrderValidationError> errors;
    const bool multileg = request.order_class == OrderClass::Mleg;

    if (multileg) {
        if (!request.symbol.empty())
            errors.push_back({"symbol", "must be omitted for mleg orders"});
        if (request.side)
            errors.push_back({"side", "must be omitted for mleg orders"});
        if (request.legs.size() < 2 || request.legs.size() > 4)
            errors.push_back({"legs", "mleg orders require 2 to 4 legs"});
    } else {
        if (request.symbol.empty())
            errors.push_back({"symbol", "is required"});
        if (!request.side)
            errors.push_back({"side", "is required"});
        if (!request.legs.empty())
            errors.push_back({"legs", "are valid only for mleg orders"});
    }

    if (request.qty.has_value() == request.notional.has_value())
        errors.push_back(
            {"qty", "exactly one of qty or notional is required"});
    RequirePositive(request.qty, "qty", errors);
    RequirePositive(request.notional, "notional", errors);
    if (request.qty && request.qty->FractionalDigits() > 9)
        errors.push_back({"qty", "supports at most 9 decimal places"});
    if (request.notional && request.notional->FractionalDigits() > 2)
        errors.push_back(
            {"notional", "supports at most 2 decimal places"});
    if (request.client_order_id.size() > 128)
        errors.push_back(
            {"client_order_id", "must not exceed 128 characters"});
    if (!multileg) {
        RequirePositive(request.limit_price, "limit_price", errors);
        RequirePositive(request.stop_price, "stop_price", errors);
    }
    RequirePositive(request.trail_price, "trail_price", errors);
    RequirePositive(request.trail_percent, "trail_percent", errors);
    if (request.take_profit &&
        request.take_profit->limit_price <= Decimal::Zero())
        errors.push_back(
            {"take_profit.limit_price", "must be greater than zero"});
    if (request.stop_loss) {
        if (request.stop_loss->stop_price <= Decimal::Zero())
            errors.push_back(
                {"stop_loss.stop_price", "must be greater than zero"});
        RequirePositive(request.stop_loss->limit_price,
                        "stop_loss.limit_price", errors);
    }

    if (request.type == OrderType::Limit && !request.limit_price &&
        request.order_class != OrderClass::Oco)
        errors.push_back({"limit_price", "is required for limit orders"});
    if (request.type == OrderType::Stop && !request.stop_price)
        errors.push_back({"stop_price", "is required for stop orders"});
    if (request.type == OrderType::StopLimit &&
        (!request.stop_price || !request.limit_price))
        errors.push_back(
            {"stop_price",
             "stop_limit orders require stop_price and limit_price"});
    if (request.type == OrderType::TrailingStop &&
        (request.trail_price.has_value() ==
         request.trail_percent.has_value()))
        errors.push_back(
            {"trail_price",
             "trailing_stop requires exactly one trail_price or trail_percent"});
    if (request.type != OrderType::TrailingStop &&
        (request.trail_price || request.trail_percent))
        errors.push_back(
            {"trail_price", "trail fields require trailing_stop type"});

    switch (request.asset_class) {
        case AssetClass::Equity:
            if (request.order_class == OrderClass::Mleg)
                errors.push_back(
                    {"order_class", "mleg is an options order class"});
            if (request.notional && request.type != OrderType::Market)
                errors.push_back(
                    {"notional", "equity notional orders must be market orders"});
            if (request.qty && !request.qty->IsIntegral() &&
                request.time_in_force != TimeInForce::Day)
                errors.push_back(
                    {"time_in_force",
                     "fractional equity orders require day time in force"});
            if (request.extended_hours &&
                (request.type != OrderType::Limit ||
                 (request.time_in_force != TimeInForce::Day &&
                  request.time_in_force != TimeInForce::Gtc)))
                errors.push_back(
                    {"extended_hours",
                     "extended-hours equities require a day or gtc limit order"});
            if ((request.time_in_force == TimeInForce::Ioc ||
                 request.time_in_force == TimeInForce::Fok ||
                 request.time_in_force == TimeInForce::Opg ||
                 request.time_in_force == TimeInForce::Cls) &&
                request.type != OrderType::Market &&
                request.type != OrderType::Limit)
                errors.push_back(
                    {"time_in_force",
                     "ioc, fok, opg, and cls equities require market or limit"});
            if (request.type == OrderType::TrailingStop &&
                (request.time_in_force != TimeInForce::Day &&
                 request.time_in_force != TimeInForce::Gtc))
                errors.push_back(
                    {"time_in_force", "trailing_stop requires day or gtc"});
            break;
        case AssetClass::Crypto:
            if (request.order_class != OrderClass::Simple)
                errors.push_back(
                    {"order_class", "crypto supports simple orders only"});
            if (request.type != OrderType::Market &&
                request.type != OrderType::Limit &&
                request.type != OrderType::StopLimit)
                errors.push_back(
                    {"type", "crypto supports market, limit, and stop_limit"});
            if (request.time_in_force != TimeInForce::Gtc &&
                request.time_in_force != TimeInForce::Ioc)
                errors.push_back(
                    {"time_in_force", "crypto supports gtc or ioc"});
            if (request.extended_hours)
                errors.push_back(
                    {"extended_hours", "is not valid for crypto"});
            break;
        case AssetClass::Option:
            if (request.notional)
                errors.push_back(
                    {"notional", "options require whole contract quantity"});
            if (request.qty && !request.qty->IsIntegral())
                errors.push_back(
                    {"qty", "option contract quantity must be whole"});
            if (request.type != OrderType::Market &&
                request.type != OrderType::Limit)
                errors.push_back(
                    {"type", "options support market or limit"});
            if (request.time_in_force != TimeInForce::Day)
                errors.push_back(
                    {"time_in_force", "options require day"});
            if (request.order_class != OrderClass::Simple &&
                request.order_class != OrderClass::Mleg)
                errors.push_back(
                    {"order_class", "options support simple or mleg"});
            if (request.extended_hours)
                errors.push_back(
                    {"extended_hours", "is not valid for options"});
            break;
        case AssetClass::Ipo:
            if (!request.notional || request.qty)
                errors.push_back(
                    {"notional", "IPO indications are notional-only"});
            if (request.side != OrderSide::Buy)
                errors.push_back({"side", "IPO indications are buy-only"});
            if (request.type != OrderType::Market)
                errors.push_back({"type", "IPO indications require market"});
            if (request.time_in_force != TimeInForce::Gtc)
                errors.push_back(
                    {"time_in_force", "IPO indications require gtc"});
            if (request.order_class != OrderClass::Simple)
                errors.push_back(
                    {"order_class", "IPO indications are simple orders"});
            break;
    }

    if (request.order_class == OrderClass::Bracket) {
        if (request.notional)
            errors.push_back({"notional", "bracket requires quantity"});
        if (request.time_in_force != TimeInForce::Day &&
            request.time_in_force != TimeInForce::Gtc)
            errors.push_back(
                {"time_in_force", "bracket requires day or gtc"});
        if (request.extended_hours)
            errors.push_back(
                {"extended_hours", "bracket does not support extended hours"});
        if (!request.take_profit || !request.stop_loss)
            errors.push_back(
                {"order_class",
                 "bracket requires take_profit and stop_loss"});
        else if (request.side == OrderSide::Buy &&
                 request.take_profit->limit_price <=
                     request.stop_loss->stop_price)
            errors.push_back(
                {"take_profit.limit_price",
                 "must be above stop_loss.stop_price for a buy bracket"});
        else if (request.side == OrderSide::Sell &&
                 request.take_profit->limit_price >=
                     request.stop_loss->stop_price)
            errors.push_back(
                {"take_profit.limit_price",
                 "must be below stop_loss.stop_price for a sell bracket"});
    } else if (request.order_class == OrderClass::Oco) {
        if (request.notional)
            errors.push_back({"notional", "oco requires quantity"});
        if (request.time_in_force != TimeInForce::Day &&
            request.time_in_force != TimeInForce::Gtc)
            errors.push_back({"time_in_force", "oco requires day or gtc"});
        if (request.extended_hours)
            errors.push_back(
                {"extended_hours", "oco does not support extended hours"});
        if (request.type != OrderType::Limit || !request.take_profit ||
            !request.stop_loss)
            errors.push_back(
                {"order_class",
                 "oco requires limit type, take_profit, and stop_loss"});
        else if (request.side == OrderSide::Sell &&
                 request.take_profit->limit_price <=
                     request.stop_loss->stop_price)
            errors.push_back(
                {"take_profit.limit_price",
                 "must be above stop_loss.stop_price for a sell OCO exit"});
        else if (request.side == OrderSide::Buy &&
                 request.take_profit->limit_price >=
                     request.stop_loss->stop_price)
            errors.push_back(
                {"take_profit.limit_price",
                 "must be below stop_loss.stop_price for a buy OCO exit"});
    } else if (request.order_class == OrderClass::Oto) {
        if (request.notional)
            errors.push_back({"notional", "oto requires quantity"});
        if (request.time_in_force != TimeInForce::Day &&
            request.time_in_force != TimeInForce::Gtc)
            errors.push_back({"time_in_force", "oto requires day or gtc"});
        if (request.extended_hours)
            errors.push_back(
                {"extended_hours", "oto does not support extended hours"});
        if (request.take_profit.has_value() ==
            request.stop_loss.has_value())
            errors.push_back(
                {"order_class",
                 "oto requires exactly one take_profit or stop_loss"});
    } else if (request.take_profit || request.stop_loss) {
        errors.push_back(
            {"order_class",
             "take_profit and stop_loss require bracket, oco, or oto"});
    }

    if (request.type == OrderType::TrailingStop &&
        request.order_class != OrderClass::Simple)
        errors.push_back(
            {"order_class", "trailing_stop is supported only as a simple order"});

    for (const OrderLeg& leg : request.legs) {
        if (leg.symbol.empty())
            errors.push_back({"legs.symbol", "is required"});
        if (leg.ratio_qty <= Decimal::Zero() ||
            !leg.ratio_qty.IsIntegral())
            errors.push_back(
                {"legs.ratio_qty", "must be a positive whole number"});
    }
    return errors;
}

std::vector<OrderValidationError> ValidateReplacement(
    const OrderState& current, const ReplaceOrderRequest& replacement) {
    std::vector<OrderValidationError> errors;
    if (current.id.empty())
        errors.push_back({"order_id", "is required"});
    if (!HasReplacementField(replacement))
        errors.push_back({"replacement", "changes no fields"});
    if (replacement.qty && replacement.notional)
        errors.push_back(
            {"qty", "qty and notional are mutually exclusive"});
    RequirePositive(replacement.qty, "qty", errors);
    RequirePositive(replacement.notional, "notional", errors);
    RequirePositive(replacement.limit_price, "limit_price", errors);
    RequirePositive(replacement.stop_price, "stop_price", errors);
    RequirePositive(replacement.trail, "trail", errors);

    if (IsProhibitedReplaceStatus(current.status))
        errors.push_back(
            {"status", "order cannot be replaced in its current status"});
    if (current.notional && current.asset_class != "ipo")
        errors.push_back(
            {"notional",
             "non-IPO notional orders must be canceled and resubmitted"});
    if (current.asset_class == "ipo") {
        if (replacement.qty)
            errors.push_back({"qty", "IPO replacement accepts notional only"});
        if (replacement.limit_price || replacement.stop_price ||
            replacement.trail || replacement.time_in_force)
            errors.push_back(
                {"replacement",
                 "IPO replacement supports notional and client_order_id only"});
    }
    if (current.asset_class == "us_equity" && current.qty &&
        !current.qty->IsIntegral() && replacement.qty &&
        *replacement.qty != *current.qty)
        errors.push_back(
            {"qty", "fractional equity quantity cannot be changed"});
    if (current.order_class == "oto")
        errors.push_back(
            {"order_class", "Alpaca does not support OTO replacement"});
    if (current.order_class == "mleg")
        errors.push_back(
            {"order_class", "Alpaca does not support mleg replacement"});
    if (replacement.trail && current.type != "trailing_stop")
        errors.push_back(
            {"trail", "trail can replace only a trailing_stop order"});
    if (replacement.qty && replacement.qty->FractionalDigits() > 9)
        errors.push_back({"qty", "supports at most 9 decimal places"});
    if (replacement.notional &&
        replacement.notional->FractionalDigits() > 2)
        errors.push_back(
            {"notional", "supports at most 2 decimal places"});
    if (replacement.client_order_id &&
        replacement.client_order_id->size() > 128)
        errors.push_back(
            {"client_order_id", "must not exceed 128 characters"});
    return errors;
}

}  // namespace tradebox::core
