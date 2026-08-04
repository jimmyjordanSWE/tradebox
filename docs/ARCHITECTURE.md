# Core architecture and V1 guarantees

This document records the durable V1 boundary. Detailed command shapes live in
[HEADLESS_APPLICATION_API.md](HEADLESS_APPLICATION_API.md), while order
semantics live in [ORDER_MODEL.md](ORDER_MODEL.md) and
[ORDER_POSITION_STATE_MACHINE.md](ORDER_POSITION_STATE_MACHINE.md).

The current post-V1 live-watchlist implementation sequence is specified in
[LUNA_LIVE_WATCHLIST_CORE_HANDOFF.md](LUNA_LIVE_WATCHLIST_CORE_HANDOFF.md).

## Authority boundaries

- Alpaca is authoritative for account, order, execution, and position state.
- The core is authoritative for local command lifecycle, reconciliation state,
  market-data projections, persistence health, and the snapshots exposed to
  clients.
- The GUI, future CLI, and future LLM interface submit typed commands and render
  immutable snapshots. They do not call Alpaca directly or invent broker state.
- Remote payloads are parsed and validated at adapter boundaries. Unknown,
  malformed, contradictory, stale-generation, or unsupported input becomes a
  typed error or safety condition instead of being silently accepted.

## Account and command flow

One account-scoped reducer serializes snapshots, stream events, and command
results. Connection generations fence late responses from earlier sessions.
The account is considered current only after the matching REST snapshot and
account-stream subscription are established and reconciliation is complete.

Broker-native commands include submit, replace, cancel, cancel-all, position
close, and emergency flattening. A command is validated and durably recorded
before external submission. Recovery replays unresolved intent without
duplicating an already acknowledged broker command. The command response
identifies validation failures, disconnection, unavailable state, broker
rejection, uncertainty, and success without requiring a UI timeout guess.

WebSocket events drive normal state changes. REST is used for initial snapshots,
command reconciliation, fills/corrections/busts, reconnect recovery, and
detected gaps—not as a high-frequency polling substitute. WebSocket loss is
immediately published as connection and safety state.

## Market-data flow

Charts request a symbol and timeframe, but persisted identity is based on the
provider's stable instrument ID. Historical coverage and gaps are durable.
Backfill is bounded and rate-aware, and errors remain visible to the caller.

Every candlestick timeframe follows the same convergence rule: the current
time bucket is provisional and may be updated by live trades/provider bars;
closed buckets are canonical history unless a provider correction explicitly
changes them. There is no special one-minute path.

Market ticks update mark-to-market projections from the latest usable price.
They do not change broker-owned quantity, cost basis, cash, buying power, or
order state.

## Persistence and observability

SQLite stores workspace state, market history and coverage, raw source events,
and command recovery records under `%LOCALAPPDATA%\TradeBox`. The render loop
does not query SQLite. Workers publish compact typed events, and clients receive
immutable snapshots.

All boundary failures are surfaced with context. This includes HTTP and
WebSocket failures, authentication/subscription loss, parsing and validation
errors, rate limits, persistence failures, stale or incomplete reconciliation,
and command outcomes whose broker result is uncertain.

## Verification

The automated suite covers deterministic reducers, disconnect/reconnect and
generation fencing, command recovery, order/position replay sequences,
market-data convergence and persistence, performance bounds, architecture
boundaries, and opt-in Alpaca Paper contract behavior.
