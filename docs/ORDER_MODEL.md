# TradeBox order model

TradeBox has two order layers. They must never be merged.

## Layer 1: broker-native orders

A broker-native order is a request that Alpaca accepts directly and an order
aggregate identified by Alpaca's immutable order ID.

The core model supports:

- Equity `market`, `limit`, `stop`, `stop_limit`, and `trailing_stop`.
- Crypto `market`, `limit`, and `stop_limit`.
- Single-leg option `market` and `limit`.
- Two-to-four-leg option `mleg` market and net debit/credit limit orders.
- Equity `simple`, `bracket`, `oco`, and `oto` classes.
- Fractional quantity and notional requests where Alpaca permits them.
- IPO notional indications of interest.
- `day`, `gtc`, `opg`, `cls`, `ioc`, and `fok`, subject to asset and order
  compatibility validation.

Replacement is represented as a new broker order, linked using `replaces` and
`replaced_by`. The old order is never mutated into the new order.

Important Alpaca restrictions encoded in core validation:

- Orders in `accepted`, `pending_new`, `pending_cancel`, or
  `pending_replace` cannot be replaced.
- A non-IPO notional order cannot be replaced; it must be canceled and
  resubmitted.
- Fractional equity quantity cannot be changed by replacement.
- OTO and multileg option replacements are rejected.
- A trailing value can replace only a `trailing_stop`.
- A successful HTTP response is provisional. The trade-update stream and
  subsequent reconciliation establish the authoritative outcome.

Current primary references:

- https://docs.alpaca.markets/us/docs/orders-at-alpaca
- https://docs.alpaca.markets/us/reference/postorder
- https://docs.alpaca.markets/us/reference/patchorderbyorderid-1
- https://docs.alpaca.markets/us/docs/options-level-3-trading
- https://docs.alpaca.markets/us/docs/crypto-orders

## Layer 2: TradeBox strategy orders

A strategy order is a TradeBox-owned intent and state machine. It may create,
replace, or cancel one or more broker-native child orders over time.

Examples include:

- Entry followed by a dynamically calculated stop.
- Scale-in and scale-out ladders.
- Timed participation or slicing.
- Breakout entry that changes with a reference range.
- Synthetic OCO across instruments.
- Position-level risk exits spanning several Alpaca orders.

Every strategy has its own immutable `strategy_id`. Every generated Alpaca
request has a unique idempotent `client_order_id` and records:

- The parent `strategy_id`.
- The strategy revision that generated it.
- Its role, such as entry, take-profit, protective stop, or hedge.
- The broker order ID after acknowledgement.
- The triggering input and exact parameters used.

The strategy state machine consumes core events; it never consumes GUI state.
Its output is a validated broker-native command. It may not call Alpaca
directly or alter broker order aggregates.

```text
GUI / CLI / LLM
       |
 typed strategy or native-order command
       |
 application command handler + risk gates
       |
 strategy reducer (only for strategy commands)
       |
 validated broker-native order command
       |
 Alpaca gateway
       |
 journaled broker events
       |
 order / position / account reducers
```

Strategy execution remains outside the V1 product boundary and disabled.
Broker-native order execution uses the command journal, idempotent client-order
IDs, replay recovery, gateway contract tests, and paper/live safety gates.
