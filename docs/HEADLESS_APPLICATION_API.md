# Headless application API

TradeBox order execution is independent of SDL, ImGui, and any particular
command source. GUI, CLI, automation, and LLM adapters submit the same typed
commands through `TradingApplication::SubmitOrder` or directly through the
injectable `OrderExecutionService`.

## Command contract

Every place, replace, cancel, cancel-all, position-close, or close-all command
carries:

- A caller-generated, globally unique `request_id`.
- A stable source name such as `gui`, `cli`, `strategy`, or `llm`.
- The expected broker account ID.
- The expected paper/live environment.
- The expected connection generation.
- Explicit live-trading confirmation for live place/replace commands.

The caller keeps the returned future or later queries
`OrderCommandStatus(request_id)`.

## Processing guarantees

The application processes commands on one serialized worker:

1. Derive an idempotent Alpaca `client_order_id` when needed.
2. Durably reserve the complete exact-decimal command payload.
3. Reject duplicate `request_id` values without calling the broker again.
4. Verify account, environment, generation, reconciliation, restrictions, and
   command-specific safety policy.
5. Validate the broker-native order or replacement.
6. Issue exactly one broker request.
7. Durably record the HTTP outcome.
8. Return a typed outcome: validation/safety/recovery rejection,
   `BrokerAccepted`, `BrokerRejected`, `PartiallyAccepted`, `Indeterminate`,
   `Duplicate`, `NotDispatched`, or `ServiceStopped`.

A transport error, timeout, HTTP 408/429/5xx response, or failure to persist
the returned outcome is `Indeterminate`. TradeBox does not automatically retry
an indeterminate order mutation. Recovery must reconcile using
`client_order_id`, broker order state, and the trade-update stream.

An HTTP success is not treated as a fill or final order transition. Broker
order state remains authoritative and changes only through journaled broker
events and reconciliation.

## Safety policy

- Place and replace require `LIVE`, reconciled, trading-permitted state.
- A live place/replace requires explicit confirmation on that command.
- Account, environment, and generation must exactly match current state.
- Cancel is allowed while the trade stream is stale or reconciling when the
  broker account remains identified. This preserves an emergency
  risk-reduction path.
- Cancel is rejected while disconnected, connecting, or in error state.
- Broker connection lifecycle and order HTTP calls share a lifecycle lock, so
  credentials cannot be switched or cleared during a command.

## Market-data projection

`TradingApplication::MarketData(symbol)` exposes a read-only quote/trade
projection containing:

- Exact bid/ask price and size.
- Exchange, condition, tape, and nanosecond broker timestamp.
- A bounded newest-first trade tape.
- Trade-ID deduplication.
- Trade correction and cancellation handling.
- Feed, stream, and quote/trade subscription health.

`TradingApplication::Bars(key, range)` exposes stable-instrument-ID,
timeframe-specific candlestick series. `ChangedMarketInstruments`,
`MarketDataChanges`, and `ChangedBarSeries` provide bounded incremental reads
for clients that do not need to copy a complete snapshot every frame.

Market snapshots may also include the optional, replayable trade-pressure
projection. It is derived from normalized trade and quote events, carries its
classification method and freshness, and can be ignored without losing market
truth. Pressure is not a broker fact and clients decide how to present its
direction, color, intensity, and neutral/stale states.

Rendering policy, chart zoom, colors, and order-form drafts belong to interface
adapters and are not part of this API.
