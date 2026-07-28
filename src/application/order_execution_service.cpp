#include "tradebox/application/order_execution_service.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>
#include <type_traits>
#include <utility>

namespace tradebox::application {
namespace {

const core::OrderCommandContext& Context(
    const core::NativeOrderCommand& command) {
    return std::visit(
        [](const auto& typed) -> const core::OrderCommandContext& {
            return typed.context;
        },
        command);
}

core::OrderCommandKind Kind(const core::NativeOrderCommand& command) {
    return std::visit(
        [](const auto& typed) {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, core::PlaceOrderCommand>) {
                return core::OrderCommandKind::Place;
            } else if constexpr (
                std::is_same_v<T, core::CancelOrderCommand>) {
                return core::OrderCommandKind::Cancel;
            } else {
                return core::OrderCommandKind::Replace;
            }
        },
        command);
}

std::string RequestId(const core::NativeOrderCommand& command) {
    return Context(command).request_id;
}

std::string DerivedClientOrderId(std::string_view request_id) {
    std::string result = "tb-";
    result.reserve(
        std::min<std::size_t>(128, result.size() + request_id.size()));
    for (unsigned char character : request_id) {
        if (result.size() == 128) break;
        if (std::isalnum(character) || character == '-' ||
            character == '_')
            result.push_back(static_cast<char>(character));
        else
            result.push_back('-');
    }
    return result;
}

std::string ValidationMessage(
    const std::vector<core::OrderValidationError>& errors) {
    std::ostringstream message;
    for (std::size_t index = 0; index < errors.size(); ++index) {
        if (index != 0) message << "; ";
        message << errors[index].field << ": " << errors[index].message;
    }
    return message.str();
}

core::OrderCommandResult Rejected(
    std::string request_id, core::OrderCommandOutcome outcome,
    std::string message) {
    return {
        .request_id = std::move(request_id),
        .outcome = outcome,
        .message = std::move(message),
    };
}

bool RequiresLiveSafety(const core::NativeOrderCommand& command) {
    return !std::holds_alternative<core::CancelOrderCommand>(command);
}

std::int64_t ToMilliseconds(
    std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               value.time_since_epoch())
        .count();
}

}  // namespace

OrderExecutionService::OrderExecutionService(
    core::ITradingCore& core, broker::IOrderGateway& gateway,
    core::IOrderCommandJournal& journal, core::IClock& clock)
    : core_(core),
      gateway_(gateway),
      journal_(journal),
      clock_(clock),
      worker_(&OrderExecutionService::WorkerLoop, this) {}

OrderExecutionService::~OrderExecutionService() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::future<core::OrderCommandResult> OrderExecutionService::Submit(
    core::NativeOrderCommand command) {
    auto pending = std::make_unique<PendingCommand>();
    pending->command = std::move(command);
    std::future<core::OrderCommandResult> result =
        pending->completion.get_future();
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) {
            pending->completion.set_value(Rejected(
                RequestId(pending->command),
                core::OrderCommandOutcome::ServiceStopped,
                "Order command service is stopped"));
            return result;
        }
        pending_.push(std::move(pending));
    }
    ready_.notify_one();
    return result;
}

std::expected<core::OrderCommandLookup, std::string>
OrderExecutionService::Lookup(const std::string& request_id) {
    if (request_id.empty())
        return std::unexpected("request_id is required");
    return journal_.Lookup(request_id);
}

void OrderExecutionService::WorkerLoop() {
    for (;;) {
        std::unique_ptr<PendingCommand> command;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock,
                        [this] { return stopping_ || !pending_.empty(); });
            if (pending_.empty() && stopping_) return;
            command = std::move(pending_.front());
            pending_.pop();
        }
        command->completion.set_value(
            Execute(std::move(command->command)));
    }
}

core::OrderCommandResult OrderExecutionService::Execute(
    core::NativeOrderCommand command) {
    const core::OrderCommandContext& context = Context(command);
    if (context.request_id.empty())
        return Rejected({}, core::OrderCommandOutcome::ValidationRejected,
                        "request_id is required");
    if (context.source.empty())
        return Rejected(context.request_id,
                        core::OrderCommandOutcome::ValidationRejected,
                        "command source is required");

    std::string client_order_id;
    if (auto* place = std::get_if<core::PlaceOrderCommand>(&command)) {
        if (place->order.client_order_id.empty())
            place->order.client_order_id =
                DerivedClientOrderId(context.request_id);
        client_order_id = place->order.client_order_id;
    } else if (auto* replace =
                   std::get_if<core::ReplaceOrderCommand>(&command)) {
        if (!replace->replacement.client_order_id)
            replace->replacement.client_order_id =
                DerivedClientOrderId(context.request_id);
        client_order_id =
            *replace->replacement.client_order_id;
    }

    const core::OrderCommandRecord record{
        .request_id = context.request_id,
        .source = context.source,
        .kind = Kind(command),
        .account_id = context.account_id,
        .environment = context.environment,
        .generation = context.generation,
        .client_order_id = std::move(client_order_id),
        .created_at_ms = ToMilliseconds(clock_.Now()),
    };
    auto reservation = journal_.Reserve(record, command);
    if (!reservation)
        return Rejected(
            context.request_id, core::OrderCommandOutcome::Indeterminate,
            "Command journal reservation failed; broker was not called: " +
                reservation.error());
    if (reservation->reservation == core::CommandReservation::Duplicate) {
        if (reservation->existing_result)
            return *reservation->existing_result;
        return Rejected(
            context.request_id, core::OrderCommandOutcome::Indeterminate,
            "Duplicate command has no durable terminal result; reconcile by "
            "client_order_id before retrying");
    }

    auto complete = [this](core::OrderCommandResult result) {
        if (auto persisted = journal_.Complete(result); !persisted) {
            result.outcome = core::OrderCommandOutcome::Indeterminate;
            result.message =
                "Broker outcome could not be durably recorded: " +
                persisted.error();
        }
        return result;
    };

    const core::CoreSnapshot snapshot = core_.Snapshot();
    if (!snapshot.account || context.account_id.empty() ||
        snapshot.account->id != context.account_id)
        return complete(Rejected(
            context.request_id, core::OrderCommandOutcome::SafetyRejected,
            "Command account does not match the active broker account"));
    if (snapshot.environment != context.environment)
        return complete(Rejected(
            context.request_id, core::OrderCommandOutcome::SafetyRejected,
            "Command paper/live environment does not match active state"));
    if (snapshot.generation != context.generation)
        return complete(Rejected(
            context.request_id, core::OrderCommandOutcome::SafetyRejected,
            "Command connection generation is stale"));

    if (RequiresLiveSafety(command)) {
        if (snapshot.safety_status != core::SafetyStatus::Live ||
            !snapshot.reconciled || !snapshot.trading_permitted)
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::SafetyRejected,
                "Place and replace require LIVE, reconciled, trading-permitted "
                "state"));
        if (context.environment == core::AccountEnvironment::Live &&
            !context.live_trading_confirmed)
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::SafetyRejected,
                "Live trading requires explicit per-command confirmation"));
    } else if (snapshot.safety_status ==
                   core::SafetyStatus::Disconnected ||
               snapshot.safety_status == core::SafetyStatus::Connecting ||
               snapshot.safety_status == core::SafetyStatus::Error) {
        return complete(Rejected(
            context.request_id, core::OrderCommandOutcome::SafetyRejected,
            "Cancel requires an identified broker session"));
    }

    broker::BrokerCommandResult broker_result;
    if (const auto* place =
            std::get_if<core::PlaceOrderCommand>(&command)) {
        const auto errors = core::ValidateOrder(place->order);
        if (!errors.empty())
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                ValidationMessage(errors)));
        broker_result = gateway_.PlaceOrder(place->order);
    } else if (const auto* cancel =
                   std::get_if<core::CancelOrderCommand>(&command)) {
        if (cancel->order_id.empty())
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "order_id is required"));
        broker_result = gateway_.CancelOrder(cancel->order_id);
    } else {
        const auto& replace = std::get<core::ReplaceOrderCommand>(command);
        const auto order = std::ranges::find_if(
            snapshot.orders, [&replace](const core::OrderState& candidate) {
                return candidate.id == replace.order_id;
            });
        if (order == snapshot.orders.end())
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                "Replacement target is absent from authoritative order state"));
        const auto errors =
            core::ValidateReplacement(*order, replace.replacement);
        if (!errors.empty())
            return complete(Rejected(
                context.request_id,
                core::OrderCommandOutcome::ValidationRejected,
                ValidationMessage(errors)));
        broker_result =
            gateway_.ReplaceOrder(replace.order_id, replace.replacement);
    }

    core::OrderCommandOutcome outcome =
        core::OrderCommandOutcome::Indeterminate;
    if (broker_result.disposition ==
        broker::BrokerCommandDisposition::Accepted)
        outcome = core::OrderCommandOutcome::BrokerAccepted;
    else if (broker_result.disposition ==
             broker::BrokerCommandDisposition::Rejected)
        outcome = core::OrderCommandOutcome::BrokerRejected;

    return complete({
        .request_id = context.request_id,
        .outcome = outcome,
        .broker_order_id = std::move(broker_result.broker_order_id),
        .http_status = broker_result.http_status,
        .message = std::move(broker_result.message),
        .raw_response = std::move(broker_result.raw_response),
    });
}

}  // namespace tradebox::application
