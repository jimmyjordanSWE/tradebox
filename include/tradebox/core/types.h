#pragma once

#include "tradebox/core/decimal.h"

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tradebox::core {

enum class AccountEnvironment {
    Paper,
    Live,
};

enum class SafetyStatus {
    Disconnected,
    Connecting,
    SnapshotLoading,
    Reconciling,
    Live,
    Stale,
    Unknown,
    Error,
};

enum class MarketDataFeed { Unknown, Iex, Sip };

struct ConnectionGeneration {
    std::uint64_t value = 0;

    auto operator<=>(const ConnectionGeneration&) const = default;
};

enum class CoreErrorCode {
    InvalidCommand,
    StaleGeneration,
    JournalFailure,
    InvalidTransition,
};

struct CoreError {
    CoreErrorCode code = CoreErrorCode::InvalidTransition;
    std::string message;
};

struct ConnectAccount {
    AccountEnvironment environment = AccountEnvironment::Paper;
};

struct DisconnectAccount {};
struct RequestReconciliation {};

using Command =
    std::variant<ConnectAccount, DisconnectAccount, RequestReconciliation>;

struct CommandReceipt {
    std::uint64_t command_id = 0;
    bool accepted = false;
    std::string message;
};

struct AccountState {
    std::string id;
    std::string account_number;
    std::string status;
    std::string crypto_status;
    std::string currency;
    std::string multiplier;
    Decimal equity;
    Decimal last_equity;
    Decimal portfolio_value;
    Decimal cash;
    Decimal buying_power;
    Decimal non_marginable_buying_power;
    Decimal regt_buying_power;
    Decimal long_market_value;
    Decimal short_market_value;
    Decimal initial_margin;
    Decimal maintenance_margin;
    Decimal last_maintenance_margin;
    Decimal sma;
    Decimal accrued_fees;
    Decimal pending_transfer_in;
    Decimal pending_transfer_out;
    bool account_blocked = false;
    bool trade_suspended_by_user = false;
    bool trading_blocked = false;
    bool transfers_blocked = false;
    bool shorting_enabled = false;
    std::int64_t received_at_ms = 0;
};

struct OrderState {
    std::string id;
    // Empty for a root order. Advanced broker orders are represented as a
    // linked tree, but stored as individually reconcilable order states.
    std::string parent_order_id;
    std::string client_order_id;
    std::string asset_id;
    std::string symbol;
    std::string asset_class;
    std::string side;
    std::string type;
    std::string time_in_force;
    std::string order_class;
    std::string status;
    std::string submitted_at;
    std::string updated_at;
    std::string filled_at;
    std::string canceled_at;
    std::string expired_at;
    std::string failed_at;
    std::string replaced_at;
    std::string replaced_by;
    std::string replaces;
    std::optional<Decimal> qty;
    std::optional<Decimal> notional;
    Decimal filled_qty;
    std::optional<Decimal> filled_avg_price;
    std::optional<Decimal> limit_price;
    std::optional<Decimal> stop_price;
    bool extended_hours = false;
    std::int64_t submitted_at_ms = 0;
    std::int64_t updated_at_ms = 0;
    std::string last_event;
};

struct PositionState {
    std::string asset_id;
    std::string symbol;
    std::string exchange;
    std::string asset_class;
    std::string side;
    Decimal qty;
    Decimal qty_available;
    Decimal avg_entry_price;
    Decimal market_value;
    Decimal cost_basis;
    Decimal unrealized_pl;
    Decimal unrealized_plpc;
    Decimal unrealized_intraday_pl;
    Decimal unrealized_intraday_plpc;
    Decimal current_price;
    Decimal lastday_price;
    Decimal change_today;
    bool provisional = false;
    bool valuation_current = false;
    bool valuation_from_market_stream = false;
    MarketDataFeed valuation_feed = MarketDataFeed::Unknown;
    std::int64_t valuation_event_time_ns = 0;
    std::int64_t valuation_received_at_ms = 0;
};

struct AccountSnapshotPayload {
    AccountState account;
};

struct PositionsSnapshotPayload {
    std::vector<PositionState> positions;
    std::int64_t received_at_ms = 0;
};

struct OrdersSnapshotPayload {
    std::vector<OrderState> orders;
    std::int64_t received_at_ms = 0;
};

struct TradeUpdatePayload {
    std::string event;
    std::string execution_id;
    OrderState order;
    std::optional<Decimal> fill_qty;
    std::optional<Decimal> fill_price;
    std::optional<Decimal> position_qty;
    std::int64_t event_at_ms = 0;
};

using BrokerPayload =
    std::variant<std::monostate, AccountSnapshotPayload,
                 PositionsSnapshotPayload, OrdersSnapshotPayload,
                 TradeUpdatePayload>;

enum class BrokerEventKind {
    ConnectionAttemptStarted,
    Authorized,
    TradeUpdatesAcknowledged,
    Disconnected,
    AccountSnapshot,
    PositionsSnapshot,
    OrdersSnapshot,
    TradeUpdate,
    ReconciliationStarted,
    ReconciliationCompleted,
    Failure,
};

struct BrokerEvent {
    BrokerEventKind kind = BrokerEventKind::Failure;
    ConnectionGeneration generation;
    std::chrono::system_clock::time_point received_at;
    std::string source_event_id;
    std::string raw_payload;
    std::string message;
    BrokerPayload payload;
};

struct CoreSnapshot {
    AccountEnvironment environment = AccountEnvironment::Paper;
    ConnectionGeneration generation;
    SafetyStatus safety_status = SafetyStatus::Disconnected;
    bool authenticated = false;
    bool trade_updates_acknowledged = false;
    bool account_snapshot_loaded = false;
    bool positions_snapshot_loaded = false;
    bool orders_snapshot_loaded = false;
    bool initial_snapshot_loaded = false;
    bool reconciled = false;
    bool trading_permitted = false;
    std::uint64_t revision = 0;
    std::string status_message = "Disconnected";
    std::optional<AccountState> account;
    std::vector<PositionState> positions;
    std::vector<OrderState> orders;
    std::int64_t positions_received_at_ms = 0;
    std::int64_t orders_received_at_ms = 0;
    std::int64_t last_trade_update_at_ms = 0;
};

}  // namespace tradebox::core
