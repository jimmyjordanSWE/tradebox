# Luna handoff: live watchlist core

## Objective

Deliver the smallest coherent core needed for the current application:

- connect to Alpaca Paper with the application-level IEX selection;
- load and edit a persistent watchlist of roughly 30 symbols;
- dynamically subscribe the required symbols;
- expose live trades, quotes, provider minute bars, freshness, recent tape, and
  persistence health;
- expose a deterministic decaying trade-pressure signal for every watchlist
  symbol;
- retain the existing account, order, position, reconciliation, and durable
  command guarantees.

The watchlist UI will use pressure direction and activity to render a pulsing
red/green light. The core must not contain color or animation policy.

## Explicit non-goals

Do not implement any of the following in this work:

- alert rules or notifications;
- a scanner DSL or historical query language;
- full-market SIP ingestion;
- raw WebSocket frame recording;
- block-pool or memory-mapped ingestion;
- SQLite tick sharding;
- one-second historical data;
- overnight processing;
- permanent exact-tick retention policy;
- an application-wide UI rewrite;
- new order behavior.

Do not delete the tested tick/replay APIs. They are optional future capability,
not the current product's storage foundation.

### Source truth versus durable history

The live normalized trade and quote events are source truth while they are in
the pipeline and in the bounded recent in-memory buffers needed for rendering,
deduplication, corrections, and derived features. They are not a promise to
retain every raw tick locally.

The durable local history for this product is the canonical one-minute bar
projection plus its coverage/presence and quality metadata. Larger timeframes
derive from those minutes. If exact trades or quotes are needed later for a
specific replay, fetch them on demand from the provider and run the same
deterministic feature code; do not create a permanent raw-tick warehouse as a
side effect of the live watchlist path.

## Existing foundation to preserve

The repository already provides:

- persistent ordered watchlist rows in SQLite;
- dynamic required-symbol calculation in the GUI;
- subscription refresh when the required symbol set changes;
- market stream authentication, acknowledgement checking, reconnect, gap
  backfill, and immediate disconnect status;
- typed trades, quotes, corrections, cancellations, status, provider bars, and
  exact decimals;
- latest canonical price, bounded recent tape, provisional live minutes, and
  generic timeframe convergence;
- historical bar pagination, coverage tracking, persistence, and error
  surfacing;
- typed application snapshots and incremental changed-instrument reads;
- asynchronous market persistence with queue/failure telemetry;
- complete account/order/position command safety.

Treat the current behavior and tests as semantic constraints. Extend them; do
not replace the market or trading core wholesale.

## Product semantics

### Feed presentation

The selected feed is application configuration and stored provenance. Display
it once in the connection/data-source surface. Do not add repetitive IEX
warning text to watchlist rows, charts, alerts, or research results.

Missing, stale, disconnected, incomplete, or failed data remains explicit.
That is operational truth, not feed-warning language.

### Subscription contents

For required watchlist/trading symbols, retain live trades and quotes because a
quote-aware pressure classification needs the applicable bid and ask. Provider
bars, updated bars, corrections, cancellations, and trading status remain
subscribed as supported by the existing adapter.

If a usable quote is unavailable, pressure may use the typed tick-direction
fallback described below. The snapshot must identify the method; it must not
pretend the fallback is a quote test.

### Pressure meaning

Use neutral market semantics:

- `BuyerInitiated`: trade at or above a usable ask.
- `SellerInitiated`: trade at or below a usable bid.
- `Unknown`: classification is not supported by the observations.

For a valid non-crossed quote, a trade strictly inside the spread may fall back
to the tick test. For a stale, future-dated, locked, or crossed quote, do not use
the quote test.

Tick test:

- above the previous distinct trade price: buyer initiated;
- below the previous distinct trade price: seller initiated;
- equal price: retain the last non-unknown tick direction only while it is
  inside the configured freshness horizon;
- otherwise: unknown.

The UI decides whether buyer/seller pressure maps to green/red and how it is
drawn.

## Derived-feature boundary

Trade pressure is not market truth. The authoritative layer is the normalized
event stream and its persisted projections: trades, quotes, provider bars,
corrections, cancellations, timestamps, source provenance, and data-quality
state. Those values must remain useful even if the pressure feature is removed,
replaced, or recalculated with a different version.

Pressure is therefore an optional, replayable derived feature. It may live in
the core process for latency and determinism, but it must be a separate module
that consumes normalized events. It must not be embedded in the Alpaca adapter,
the canonical market-data rows, or the UI rendering code. The same module must
be usable for live events and historical replay, with configuration and feature
version explicit in its output.

Suggested files:

```text
include/tradebox/core/trade_pressure.h
src/core/trade_pressure.cpp
tests/unit/trade_pressure_tests.cpp
```

Suggested public types (names may be refined without changing semantics):

```cpp
enum class TradeInitiation {
    Unknown,
    BuyerInitiated,
    SellerInitiated,
};

enum class TradeClassificationMethod {
    Unknown,
    QuoteTest,
    TickTest,
};

enum class TradePressureWeight {
    Trades,
    Shares,
    Notional,
};

struct TradePressureConfig {
    std::int64_t half_life_ms = 500;
    std::int64_t quote_max_age_ms = 2'000;
    std::int64_t tick_direction_max_age_ms = 5'000;
    std::int64_t correction_horizon_ms = 30'000;
    std::size_t maximum_recent_contributions = 512;
    TradePressureWeight weight = TradePressureWeight::Shares;
};

struct TradePressureSnapshot {
    double signed_pressure = 0.0; // [-1,+1]
    double activity = 0.0;        // [0,1]
    double buyer_weight = 0.0;
    double seller_weight = 0.0;
    double unknown_weight = 0.0;
    std::uint64_t buyer_trades = 0;
    std::uint64_t seller_trades = 0;
    std::uint64_t unknown_trades = 0;
    TradeClassificationMethod latest_method =
        TradeClassificationMethod::Unknown;
    std::int64_t last_observation_ms = 0;
    std::int64_t half_life_ms = 500;
    bool stale = true;
};
```

The reducer API takes explicit observation times. It must not read the system
clock internally. Unit tests and replay must produce identical results.

### Decay

Maintain separate decaying buyer and seller accumulators. Before applying an
observation at `now`:

```text
decay = 2 ^ (-(now - previous_time) / half_life)
buyer *= decay
seller *= decay
unknown *= decay
```

Then add the classified weight. Define:

```text
gross = buyer + seller
signed_pressure = gross > 0 ? (buyer - seller) / gross : 0
activity = bounded_saturation(gross)
```

Saturation must be documented and configurable or derived from a slowly
adapting activity baseline. Start with the simplest deterministic bounded
formula and benchmark before adding adaptive complexity.

The snapshot carries `last_observation_ms` and `half_life_ms`, allowing the UI
to decay brightness between events without mutating core state or generating a
core event every render frame.

### Corrections and cancellations

Retain a bounded deque of recent classified contributions keyed by the same
day-qualified trade identity used by `MarketDataStore`. A cancellation or
correction inside the configured horizon removes/replaces the contribution and
rebuilds the transient accumulators deterministically. Outside the horizon,
canonical price and bar correction behavior remains unchanged, but past visual
pulses are not reconstructed.

Memory remains bounded for every symbol.

## Integration boundary

Keep `MarketDataStore` authoritative for normalized source events and canonical
market projections. Attach the pressure reducer through a derived-feature
boundary (for example, a feature consumer fed by accepted trade/quote events),
not by making pressure part of the source record schema.

- Quote events update the feature's usable-quote context.
- Accepted/deduplicated trades are offered to the feature once.
- Corrections and cancellations are offered to the feature when they affect a
  contribution inside its bounded correction horizon.
- Out-of-order observations must not rewind feature decay time.
- Stream disconnect/feed change marks the feature stale and clears its
  classification context consistently with the existing live projection
  boundary.

Expose the result as an optional derived snapshot alongside the existing market
snapshot (or through a generic feature-snapshot collection). Preserve existing
source snapshot and delta semantics. A feature change may use the existing
changed-instrument ring; do not add a second polling channel. Consumers must be
able to ignore the feature and still receive complete market truth.

Do not persist decayed pressure or require durable raw trades/quotes for it. It
is derived transient state over the bounded live event window. Persist the
canonical minute/bar projections and their data-quality metadata; exact raw
events are fetched on demand when a replay explicitly needs them.

## Implementation sequence

Each step must compile and pass its focused tests before proceeding. Keep
commits small enough to revert independently.

### Step 1: baseline and contract tests

1. Build Release from the handoff commit.
2. Run all tests and record the count.
3. Add failing unit tests for classification, decay, correction, cancellation,
   staleness, and bounded memory.
4. Do not change production behavior in this step.

Acceptance: existing tests still pass; new tests compile and fail only for the
missing pressure implementation.

### Step 2: pure pressure reducer

Implement the standalone reducer with no SQLite, Alpaca, GUI, or application
dependencies.

Required tests:

- at-ask and above-ask trade is buyer initiated;
- at-bid and below-bid trade is seller initiated;
- stale/future/locked/crossed quote is not used;
- inside-spread trade uses the documented tick fallback;
- equal-price tick direction expires;
- pressure is bounded in `[-1,+1]`;
- activity decays monotonically;
- a burst increases activity;
- unknown observations do not invent direction;
- out-of-order observation time cannot reverse decay;
- contribution storage remains bounded;
- correction replaces and cancellation removes a recent contribution.

Acceptance: the reducer is deterministic and all focused tests pass.

### Step 3: derived-feature integration

Connect the reducer to accepted normalized trade/quote events through the
smallest existing event/projection boundary. Do not alter canonical trade or
quote storage to carry pressure fields. Test both single-event and batch
ingestion.

Required tests:

- quote then trade publishes the derived pressure result alongside the latest
  price;
- duplicate trade does not add duplicate pressure;
- correction/cancellation updates price, provisional minute, tape, and pressure
  coherently;
- reconnect/feed change marks pressure stale;
- changed-instrument cursor reports pressure changes;
- consumer overrun retains existing gap behavior.

Acceptance: no new adapter dependency enters the feature module; canonical
market-data tests remain source-focused; every integration test passes.

### Step 4: application/watchlist surface

Expose pressure through the existing `TradingApplication::MarketData` snapshot.
Do not add a database query to the render loop.

Confirm the existing flow:

```text
persistent watchlist
  -> required-symbol union
  -> RefreshMarketSymbols
  -> exact subscription acknowledgement
  -> MarketDataStore
  -> changed-instrument cursor
  -> immutable watchlist snapshots
```

Add or strengthen tests for watchlist ordering and persistence across reopen.
If `SaveWatchlist` encounters a database failure, make the failure visible to
the application rather than silently reporting success.

Acceptance: a watchlist edit survives restart and changes the live subscription
without reconnecting the account.

### Step 5: UI integration

This is a narrow addition to the existing watchlist, not the planned UI
rewrite.

For each visible symbol:

- direction selects the presentation side/color;
- decayed activity selects brightness/opacity;
- stale/unknown is visibly neutral;
- rendering interpolates smoothly and never blinks the whole application;
- UI refresh frequency does not create network, core, or database work.

Acceptance: a synthetic deterministic event stream produces the expected pulse
and fades to neutral with no additional events.

### Step 6: paper verification

Use the authorized Alpaca Paper environment with approximately 30 symbols.

Verify:

- authentication and exact subscription acknowledgement;
- live price/tape/pressure updates;
- provider minute convergence;
- add/remove watchlist symbol while connected;
- disconnect becomes stale immediately;
- reconnect resumes without duplicate pressure contributions;
- watchlist and bars survive restart;
- persistence failures and queue overload remain visible;
- no dropped market events.

Record observed bandwidth, events/s, CPU, memory, queue high-water marks, and
persistence health. Do not extrapolate SIP requirements from this test.

Acceptance: the current live trading application is usable with the persistent
watchlist and no silent error path.

## Minute research storage contract (design now, implement later)

Do not discard the compression design merely because exact ticks are deferred.
The eventual immutable symbol-day minute chunk contains:

```text
header:
  schema version
  stable instrument id
  session date/calendar id
  feed and vendor
  adjustment policy
  fixed-point scale
  checksum

bitmaps:
  covered minute
  present/traded minute
  optional VWAP
  quality flags

packed present bars:
  close delta from previous close/keyframe
  open delta from previous close
  upper wick
  lower wick
  unsigned volume
  unsigned trade count
  optional VWAP delta
```

Two bitmaps are mandatory: covered/no-trade is not the same as missing. Time is
implicit on the session grid. Periodic absolute keyframes bound corruption and
random-access decode work. Larger timeframes and indicators derive from this
one-minute source and may be cached, not duplicated as canonical data.

The current SQLite provider-bar store remains valid until a representative
research corpus proves that the immutable chunk store is needed.

## Luna execution rules

- Work one numbered step at a time.
- Read this document and the referenced core headers before editing.
- Preserve unrelated user changes in the working tree.
- Use `apply_patch` for source edits.
- Run focused tests after each edit and all tests before handoff.
- Never weaken persistence, disconnect, correction, or error surfacing to make
  a test pass.
- Never add silent event dropping.
- Do not commit or push unless the user explicitly asks.
- Update `PERFORMANCE.txt` only for repeated measured experiments, including
  rejected results.
- Stop and report if the requested step requires expanding a stated non-goal.

## Suggested first prompt for Luna

```text
Read docs/LUNA_LIVE_WATCHLIST_CORE_HANDOFF.md completely. Implement only Step 1
and Step 2: the standalone deterministic trade-pressure reducer and its unit
tests. Preserve all existing semantics and unrelated changes. Build and run the
focused tests, then run the full suite. Do not integrate it into MarketDataStore
yet, and do not commit or push.
```
