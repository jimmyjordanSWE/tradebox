#pragma once

#include "tradebox/broker/gateway.h"
#include "tradebox/core/interfaces.h"
#include "tradebox/core/order_command.h"

#include <condition_variable>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace tradebox::application {

class OrderExecutionService final {
public:
    OrderExecutionService(core::ITradingCore& core,
                          broker::IOrderGateway& gateway,
                          core::IOrderCommandJournal& journal,
                          core::IClock& clock);
    ~OrderExecutionService();

    OrderExecutionService(const OrderExecutionService&) = delete;
    OrderExecutionService& operator=(const OrderExecutionService&) = delete;

    [[nodiscard]] std::future<core::OrderCommandResult> Submit(
        core::NativeOrderCommand command);
    [[nodiscard]] std::expected<core::OrderCommandLookup, std::string>
    Lookup(const std::string& request_id);

private:
    struct PendingCommand {
        core::NativeOrderCommand command;
        std::promise<core::OrderCommandResult> completion;
    };

    void WorkerLoop();
    core::OrderCommandResult Execute(core::NativeOrderCommand command);

    core::ITradingCore& core_;
    broker::IOrderGateway& gateway_;
    core::IOrderCommandJournal& journal_;
    core::IClock& clock_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::unique_ptr<PendingCommand>> pending_;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace tradebox::application
