# Order and position consistency design

## Scope and safety rule

The broker is the source of truth. The application may display an optimistic
local state, but it must never describe that state as safe/current unless all
of these conditions hold:

1. The account identity and paper/live environment are known.
2. A complete REST snapshot has been accepted for the current connection
   generation.
3. The `trade_updates` WebSocket is authenticated and the server has
   acknowledged `trade_updates` in a `listening` message.
4. Every update received since the snapshot has been applied exactly once, or
   the stream has been interrupted and reconciliation has completed.
5. No reconciliation request is in flight or failed.

If any condition is false, the state is displayed as `UNKNOWN`,
`RECONCILING`, or `STALE`; it is not silently presented as live.

## Ownership and event flow

There should be one account-scoped actor owning order and position state. REST
workers and WebSocket workers only parse messages and enqueue typed commands;
they do not mutate UI vectors or shared models directly.

```text
REST snapshot / WebSocket frame / command result
                 |
                 v
       account event queue (ordered)
                 |
                 v
       OrderPositionState actor
          |                 |
          v                 v
    durable journal     immutable UI snapshot
```

The actor is serialized per account and connection generation. Every command
has a monotonically increasing local sequence number. Every broker event is
recorded before it is applied, with its raw payload, received time, event type,
order ID, execution ID, and generation.

## Connection generations

On connect or reconnect, increment `generation` and clear the stream
acknowledgement for the old generation. A snapshot belongs to the generation
that requested it. A response from an older generation must be discarded.

The account stream state is:

```text
DISCONNECTED -> CONNECTING -> AUTHENTICATED -> LISTENING -> LIVE
                     |              |              |
                     +--------------+--------------+-> RECONNECT_BACKOFF
```

`LIVE` requires both a completed initial snapshot and the `listening` ack for
the same generation. A healthy socket with no order events remains `LIVE`; an
event-age timer is informational, not a freshness failure. Any close, receive
error, auth failure, subscription failure, or heartbeat timeout moves the
state to `RECONCILING`/`RECONNECT_BACKOFF`, marks orders and positions stale,
and starts reconnect with bounded exponential backoff and jitter.

After reconnect, REST reconciliation must complete before returning to `LIVE`.
The safe sequence is: establish stream, request snapshot, accept snapshot,
apply buffered updates for that generation, compare against REST, then publish
`LIVE`. If the API cannot provide an event cursor, the comparison is mandatory
because a WebSocket disconnect can lose events.

## Order state machine

The order record is keyed by broker `id`, with `client_order_id` as a secondary
lookup. A replacement creates a new broker order and links old/new IDs; it is
not an in-place mutation of identity.

Accepted broker event names include:

`new`, `accepted`, `pending_new`, `partially_filled`, `partial_fill`, `fill`,
`pending_cancel`, `canceled`, `pending_replace`, `replaced`, `expired`,
`done_for_day`, `rejected`, `stopped`, `suspended`, `calculated`, `held`,
`order_cancel_rejected`, `order_replace_rejected`, `trade_bust`, and
`trade_correct`.

Unknown event names are persisted and surfaced as `UNHANDLED_EVENT`; they must
not be ignored or guessed into a state transition.

Rules:

- `fill` and `partial_fill` are applied by `execution_id` idempotency. A
  repeated execution ID is an acknowledged duplicate and changes nothing.
- Filled quantity must be monotonic for normal fill events and may never exceed
  ordered quantity unless the broker payload is quarantined for reconciliation.
- The event's full order object is the proposed broker state. It is accepted
  only if its `updated_at`/event timestamp is not older than the stored state,
  except for a duplicate execution event.
- `done_for_day` is `PAUSED_UNTIL_NEXT_SESSION`, not terminal.
- `canceled`, `expired`, `rejected`, and `filled` are terminal for that broker
  order ID, but a later correction/bust can require reconciliation.
- `pending_cancel` and `pending_replace` keep the order actionable only for
  status display; the command layer must prevent conflicting duplicate
  requests unless explicitly forced.
- `order_cancel_rejected` and `order_replace_rejected` clear the pending
  command and retain the prior executable order state from the broker payload.
- A fill always triggers immediate position and account reconciliation. The
  stream's `position_qty` is useful for latency-sensitive display, but is not
  the sole authoritative position record.

## Position state machine

Positions are keyed by `asset_id` (symbol is only a display/lookup alias).
Normal operation is:

```text
NO_SNAPSHOT -> SNAPSHOT_CURRENT -> STREAM_DIRTY -> RECONCILING
                                      ^                 |
                                      +---- fill -------+
```

The fill event may update a provisional quantity immediately, but the UI must
label average price, cost basis, buying power, and complete position data as
`PENDING_RECONCILIATION` until `/v2/positions` and `/v2/account` succeed.
There is no need to poll every two seconds. Use immediate reconciliation after
fills/corrections/busts, after reconnect, and a one-second safety reconciliation
while actively trading or while the stream is unavailable. When the account is
idle, this can be relaxed to 30–60 seconds with backoff on errors. Market-data
ticks update only mark-to-market fields; they do not change position quantity
or cost basis.

An empty successful `/v2/positions` response is a valid current snapshot and
must replace the prior set. A failed or partial response must never erase the
last known positions.

## Numeric representation

Broker decimal strings must not pass through `double` or `std::stod`.

- Store the original canonical decimal text for audit/replay.
- Use a fixed-point decimal type for arithmetic: signed integer coefficient
  plus explicit scale, with checked overflow and exact add/subtract/multiply.
- Quantities and prices need independent scales because quantity precision and
  price precision differ by asset and broker rules. Load asset metadata and
  validate `min_order_size`, `min_trade_increment`, and `price_increment` before
  submission.
- Use integer cents (or an explicit currency scale) for account money where the
  API contract guarantees that scale; retain the raw decimal for fields whose
  precision can vary.
- Formatting for the UI happens only at the boundary. Never compare monetary or
  quantity values after conversion to binary floating point.
- Market bars may remain a separate analytics type initially, but they must not
  feed order, position, risk, or buying-power decisions without exact conversion
  and explicit rounding policy.

## Current implementation findings

The current app is a read-only monitoring scaffold, not yet an order execution
system. It currently:

- stores all account, position, order, and bar numerics as `double`;
- reloads all orders through REST after a dirty flag instead of applying stream
  deltas;
- has no account-stream reconnect loop or generation fencing;
- uses a one-second position safety check and a one-second full-order fallback
  only while the trading stream is unavailable;
- refreshes the account snapshot every five seconds for buying power, cash,
  equity, and account restrictions;
- does not carry the order/event payload through `UiEvent` to a state reducer;
- has no fill idempotency ledger, position reconciliation barrier, durable order
  command journal, or command/result correlation;
- uses the initial REST load timestamp as the order snapshot age even after
  stream updates, which is acceptable only when clearly labelled as snapshot
  age and not stream freshness.

Until the reducer, reconciliation barrier, exact numeric model, and reconnect
logic are implemented and tested with replayed event sequences, the application
must remain read-only. These are correctness prerequisites for adding order
submission, cancel, or replace controls.

## Required test matrix

Replay tests must cover: duplicate fill; out-of-order partial fill; fill during
cancel; cancel rejection; replace success; replace rejection; old order filled
while replacement is pending; done-for-day followed by next-day new; rejected
order; empty position snapshot; snapshot arriving before/after buffered fill;
disconnect during fill; reconnect with a missing event; trade bust/correct;
unknown event; paper/live account switch; and REST failure after a fill.

Every test should assert both the resulting state and the published safety
status. A visually correct order table is not sufficient evidence of
correctness.
