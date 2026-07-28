#pragma once

#include "tradebox/core/interfaces.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tradebox::core {

class TradingCore final : public ITradingCore {
public:
    TradingCore(IEventJournal& journal, IClock& clock);

    CoreSnapshot Snapshot() const override;
    std::expected<CommandReceipt, CoreError> Submit(Command command) override;
    std::expected<void, CoreError> Ingest(BrokerEvent event) override;

private:
    std::expected<void, CoreError> ApplyOrdersSnapshot(
        const OrdersSnapshotPayload& snapshot);
    std::expected<void, CoreError> ApplyTradeUpdate(
        const TradeUpdatePayload& update);
    void PublishOrders();
    void PublishPositions();
    void RefreshSafetyStatus();
    void ResetConnectionState(SafetyStatus status, std::string message);
    void ClearFinancialState();

    IEventJournal& journal_;
    IClock& clock_;
    mutable std::mutex mutex_;
    CoreSnapshot state_;
    std::unordered_map<std::string, OrderState> orders_by_id_;
    std::unordered_map<std::string, PositionState> positions_by_asset_id_;
    std::unordered_set<std::string> execution_ids_;
    std::vector<TradeUpdatePayload> buffered_trade_updates_;
    bool reconciliation_required_ = false;
    std::uint64_t next_command_id_ = 1;
};

}  // namespace tradebox::core
