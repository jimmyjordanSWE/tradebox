# Order and position state-machine implementation plan

This plan is the execution checklist for converting the current read-only
monitor into a deterministic, broker-authoritative order and position system.
Order submission, cancel, and replace controls remain disabled until the
release gates at the end are checked.

## Target architecture

The graphical interface is an adapter, not the trading application. The core
must be usable without SDL, ImGui, Windows UI code, or a running desktop.

```text
CLI / GUI / LLM adapter / future automation
                 |
          application service API
                 |
       deterministic trading core
                 |
       broker gateway interfaces
                 |
     Alpaca REST + WebSocket adapters
```

Ownership rules:

- The core owns order, position, account, connection, reconciliation, command,
  and safety state.
- The core is the only component allowed to mutate financial state.
- Broker adapters own sockets, HTTP, retries, response parsing, and rate-limit
  metadata; they do not decide business-state transitions.
- Persistence owns journal/snapshot storage; it does not interpret broker
  events.
- UI/CLI/LLM adapters submit validated commands and consume read-only immutable
  projections. They never access broker clients or mutate core state directly.
- The LLM adapter, if added, receives capabilities and typed projections only;
  it cannot issue arbitrary HTTP requests or bypass command validation.

Required core interfaces:

```cpp
class ITradingCore {
public:
    virtual CoreSnapshot Snapshot() const = 0;
    virtual CommandReceipt Submit(Command command) = 0;
    virtual void Ingest(BrokerEvent event) = 0;
};

class IBrokerGateway {
public:
    virtual BrokerResult SubmitOrder(const NewOrder& order) = 0;
    virtual BrokerResult CancelOrder(OrderId id) = 0;
    virtual BrokerResult ReplaceOrder(OrderId id, const ReplaceOrder& change) = 0;
    virtual void Start(BrokerEventSink sink) = 0;
    virtual void Stop() = 0;
};
```

The concrete implementation may use different names, but the dependency
direction must remain the same: adapters depend on interfaces, while the core
does not depend on GUI, transport, JSON, WinHTTP, SDL, or ImGui.

## Product-grade repository structure

The current flat `src/` executable layout is prototype-scale. The target
layout is:

```text
trade_box_native/
  CMakeLists.txt
  cmake/
  include/tradebox/
    core/          public domain and application interfaces
    broker/        broker gateway contracts and DTOs
    persistence/   journal and snapshot contracts
    adapters/      GUI/CLI-facing read and command contracts
  src/
    core/          reducers, aggregates, commands, safety state
    broker/
      alpaca/      REST/WebSocket implementation and parsing
    persistence/  SQLite journal/snapshot implementation
    adapters/
      gui/         SDL/ImGui application adapter
      cli/         terminal adapter
  tests/
    unit/          decimal, reducers, transitions, commands
    integration/   fake broker, journal recovery, reconnects
    contract/      Alpaca payload and schema compatibility
    replay/        production event-log replay fixtures
  tools/
    replay/        headless journal/replay executable
    cli/           CLI executable
  docs/
```

The GUI executable should become a thin composition root: construct concrete
adapters, wire interfaces, run the event loop, and render projections. It
should not contain broker parsing, order transitions, financial arithmetic, or
reconciliation policy.

## Phase 0 — Freeze the safety contract

- [x] Change the project target to C++23 and document the supported compiler
      versions and standard-library requirements.
- [x] Add separate `tradebox_core`, `tradebox_broker_alpaca`,
      `tradebox_persistence`, `tradebox_platform`, and `tradebox_gui` CMake
      targets.
- [ ] Add `include/`, `tests/`, `tools/`, and `docs/` directory boundaries.
- [x] Add CTest/GoogleTest or an equivalent lightweight unit-test harness.
- [ ] Add warnings-as-errors for core targets and sanitizer/static-analysis
      configurations for test builds.
- [ ] Add CI build, unit-test, integration-test, and formatting checks.
- [x] Create a transport-independent `core` module/library.
- [ ] Move all order, position, account, connection, reconciliation, and
      command state out of `main.cpp`.
- [x] Define `CoreSnapshot`, typed `Command`, `CommandReceipt`, and
      `BrokerEvent` interfaces.
- [x] Define `IBrokerGateway`, `IEventJournal`, and `IClock` test seams.
- [ ] Make GUI, CLI, and future LLM surfaces depend only on the application
      service API and immutable snapshots.
- [x] Add compile-time/build boundaries preventing core code from including
      SDL, ImGui, WinHTTP, or Windows UI headers.
- [ ] Confirm paper/live account identity is part of every state key.
- [ ] Define `connection_generation`, snapshot generation, stream generation,
      and event sequence fields.
- [x] Define explicit safety statuses: `DISCONNECTED`, `CONNECTING`,
      `SNAPSHOT_LOADING`, `RECONCILING`, `LIVE`, `STALE`, `UNKNOWN`, and
      `ERROR`.
- [ ] Define which statuses are terminal, resumable, provisional, or require
      REST reconciliation.
- [ ] Make the broker the only authority for executable order and position
      state.
- [ ] Preserve the existing read-only behavior during all intermediate phases.

## Phase 1 — Exact financial domain types

- [x] Add a checked fixed-point decimal type with signed coefficient and scale.
- [x] Parse Alpaca decimal strings without `double`, `std::stod`, or binary
      floating-point intermediate values.
- [x] Preserve canonical raw decimal text for audit and replay.
- [ ] Define separate quantity, price, percentage, and currency representations.
- [ ] Add checked arithmetic, comparison, normalization, and formatting tests.
- [x] Replace `double` in order, position, account, and risk fields.
- [x] Keep chart/analytics doubles isolated from trading decisions.
- [ ] Validate fractional quantity precision and asset increments from broker
      metadata before any future order submission.

## Phase 2 — Typed event envelope and durable journal

- [x] Add typed events for REST snapshots, trade updates, fills, corrections,
      busts, connection transitions, reconciliation results, and commands.
- [ ] Include account ID, paper/live mode, generation, receive time, broker
      timestamp, order ID, execution ID, and raw payload.
- [x] Add a durable append-only journal before state mutation.
- [ ] Make journal writes crash-safe and detect duplicate event IDs.
- [x] Add bounded handling for malformed JSON, unknown event types, and missing
      required identifiers.
- [ ] Ensure all state mutations happen on one serialized account actor/queue.

## Phase 3 — Order aggregate and reducer

- [ ] Key orders by broker order ID; maintain client-order ID as a secondary
      index.
- [x] Implement all documented lifecycle events, including `held`,
      `done_for_day`, `calculated`, suspended, cancel/replace rejection, and
      trade correction/bust events.
- [x] Treat the complete order object in each broker event as proposed state.
- [x] Reject or quarantine impossible transitions instead of guessing.
- [x] Make fills idempotent by `execution_id`.
- [x] Enforce monotonic filled quantity for ordinary fill events.
- [x] Reject filled quantity greater than ordered quantity unless reconciliation
      confirms the broker state.
- [ ] Link replacement orders using `replaces`/`replaced_by`; never mutate order
      identity in place.
- [ ] Track pending cancel/replace commands and correlate their outcomes.
- [ ] Mark stale/out-of-order updates and request reconciliation.

## Phase 4 — Position aggregate and fill barrier

- [x] Key positions by asset ID, with symbol as a display alias.
- [x] Apply fill `position_qty` as provisional low-latency state only.
- [x] Mark account/position state `PENDING_RECONCILIATION` after every fill,
      correction, or bust.
- [x] Fetch `/v2/positions` and `/v2/account` immediately after those events.
- [ ] Atomically publish the reconciled position/account snapshot.
- [x] Preserve the last valid snapshot when a REST request fails.
- [x] Treat a successful empty positions response as a valid zero-position
      snapshot.
- [ ] Keep market-data mark-to-market updates separate from quantity and cost
      basis.

## Phase 5 — Stream lifecycle and reconciliation

- [ ] Implement reconnect with bounded exponential backoff and jitter.
- [x] Increment generation on every connection attempt.
- [x] Require authorization and `listening` acknowledgement for `LIVE`.
- [x] Buffer or quarantine events until the matching REST snapshot is accepted.
- [ ] Discard responses and events from older generations.
- [ ] Reconcile orders, positions, and account after every reconnect.
- [ ] Detect receive errors, close frames, auth errors, subscription errors,
      and heartbeat timeouts.
- [ ] Distinguish “no events occurred” from “stream is unhealthy”.
- [x] Maintain one-second position safety reconciliation and five-second account
      safety reconciliation during active use.
- [x] Use one-second full-order REST fallback only while the order stream is
      unavailable; do not repeatedly download order history while the stream is
      healthy.
- [ ] Read `X-RateLimit-Limit`, `X-RateLimit-Remaining`, and
      `X-RateLimit-Reset` from every response.
- [ ] Back off on HTTP 429 and expose rate-limit state in diagnostics.

## Phase 6 — UI integration

- [x] Replace mutable UI-owned order/position vectors with immutable published
      state snapshots.
- [ ] Display stream state, snapshot state, reconciliation state, and data age
      separately.
- [ ] Show explicit messages for provisional fills, stale snapshots, reconnect,
      rejected transitions, and unknown events.
- [x] Never show `LIVE` unless snapshot, subscription acknowledgement, and
      reconciliation barriers all pass.
- [ ] Disable all trading controls when state is stale, unknown, reconciling,
      or account restrictions are unresolved.
- [x] Display exact decimal values without binary-float formatting artifacts.

## Phase 7 — Replay and failure testing

- [ ] Add deterministic reducer unit tests for every event/status.
- [ ] Test duplicate fill and duplicate trade update.
- [ ] Test out-of-order partial fill and stale order object.
- [ ] Test fill during pending cancel.
- [ ] Test cancel success and cancel rejection.
- [ ] Test replacement success and replacement rejection.
- [ ] Test old order fills while replacement is pending.
- [ ] Test `done_for_day` followed by next-session activity.
- [ ] Test rejected, held, stopped, suspended, calculated, and unknown events.
- [ ] Test empty position snapshot and REST failure after a fill.
- [ ] Test disconnect during fill and reconnect with a missing event.
- [ ] Test trade bust and trade correction.
- [ ] Test generation fencing during overlapping REST requests.
- [ ] Test paper/live account switching and stale queued events.
- [ ] Test decimal overflow, precision, rounding, and fractional increments.
- [ ] Add property tests for idempotency and replay determinism.

## Release gates

- [ ] All replay tests pass from a clean process and from journal recovery.
- [ ] Replaying the same journal produces byte-equivalent financial state.
- [ ] No order/position/account field used for trading decisions is a `double`.
- [ ] No unhandled event can silently mutate executable state.
- [ ] Reconnect and reconciliation are observable and tested.
- [ ] Rate-limit headers and 429 backoff are implemented and tested.
- [ ] UI safety status is correct for every failure test.
- [ ] Paper trading soak test passes through disconnects, fills, cancels, and
      reconnects.
- [ ] Only after all gates pass: implement order submission behind an explicit
      paper/live safety confirmation and idempotent client order IDs.
