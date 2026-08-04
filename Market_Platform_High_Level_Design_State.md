# Market Data, Research, Scanner, and Replay Platform

## Purpose

Build one local-first market platform combining live Alpaca ingestion, whole-market scanning, historical one-minute research, backtesting, realistic replay, selective tick analysis, and live decision support.

The system must support broad historical questions over many years while also allowing deep inspection of selected moments at sub-minute resolution.

## Current Delivery Scope (August 2026)

The immediate product is deliberately smaller than the eventual platform:

- Alpaca IEX is selected once at the application level. Feed provenance remains
  stored with data, but ordinary screens do not repeat warning language.
- A persistent watchlist of roughly 30 symbols receives live trades, quotes,
  provider minute bars, corrections, cancellations, and trading status.
- The core exposes latest price, current/finalized bars, stream freshness,
  recent trades, and a decaying trade-pressure signal suitable for a pulsing
  watchlist visualization.
- Existing account, order, position, reconciliation, and persistence safety
  remain intact.
- One-minute bars are the lowest permanent research resolution and the source
  for larger intraday timeframes and bar-based indicators.
- No alert engine, overnight processing, full-market SIP archive, raw-frame
  recorder, one-second database, or permanent exact-tick archive is required
  in the current slice.
- Exact historical trades and quotes may later be fetched for one symbol-day
  on demand and cached as a replay package.
- Raw ticks are not part of the durable local database for the current product.
  The live pipeline may keep a bounded in-memory event window for correction,
  rendering, and feature calculation, while canonical one-minute projections
  provide permanent local history.

Tested tick/replay APIs may remain available for future on-demand replay, but
the live watchlist path must not persist every tick as a side effect.

## Core Principle

Do not force every workload onto one data representation.

Use separate resolution layers:

1. **Daily/session data** for coarse conditions and indexes.
2. **One-minute bars** as the permanent universal research corpus.
3. **One-second summaries** for selected setups and deeper review.
4. **Exact trades and quotes** for replay, live use, and narrow event windows.
5. **Raw Alpaca frames** temporarily for recovery and reprocessing.

Tick data is a zoom layer, not the default historical database.

## System Shape

```text
Alpaca live and historical APIs
            ↓
Normalized live event model (bounded in memory)
            ↓
Canonical one-minute projections and quality metadata
            ↓
┌────────────────────────────────────────────┐
│ Live scanner and market-state projection   │
│ Tick and quote replay                      │
│ One-second event summaries                 │
│ One-minute research bars                   │
│ Daily/session summaries and indexes        │
└────────────────────────────────────────────┘
            ↓
Historical query engine, backtester, and UI
```

## Data Sources

Start with **Alpaca IEX** for development and historical backfill. Prioritize:

- QQQ constituents
- Repeated top movers and news names
- Important liquid stocks
- Broad market, sector, industry, bond, commodity, and volatility proxies

Minute history is required for context instruments such as SPY, QQQ, IWM, DIA, RSP, sector ETFs, SMH, XBI, KRE, XRT, TLT, HYG, GLD, and USO because research conditions often depend on their state at the exact same minute.

The architecture must support Alpaca SIP later without changing the internal data model.

Every dataset stores provenance:

```text
vendor
feed
coverage
schema version
adjustment policy
quality flags
```

## Live Ingestion

```text
WebSocket receiver
        ↓
Bounded decoder pool
        ↓
Immutable normalized event blocks
        ↓
Stable instrument routing
        ├── Scanner lanes
        ├── Market-state lanes
        ├── Bar builders
        ├── Replay capture
        └── Persistence
```

Hot-path rules:

- No per-event heap allocation
- No strings for closed-set fields
- Process contiguous blocks
- Preserve per-instrument ordering
- Parallelize across instruments
- Move block handles through queues, not individual events
- Keep socket, scanner, UI, and storage independent

Raw capture stores original Alpaca frames with connection generation, frame sequence, receive timestamp, payload length, checksum, original bytes, and recovery boundary. It is temporary insurance against parser or schema errors.

## Canonical Event Model

Typed event families:

- Trade
- Quote
- Trade correction
- Trade cancellation
- Bar
- Trading status
- LULD update
- Corporate action
- Calendar/session event

Use stable integer instrument IDs. Tickers are mutable labels, not permanent identities.

## Permanent Historical Store

### One-minute core

```text
open
high
low
close
volume
trade count
VWAP
```

Optional minute microstructure summaries when SIP is available:

```text
volume at bid
volume at ask
unknown-side volume
largest trade
quote count
time-weighted average spread
spread percentiles
average displayed bid/ask size
average NBBO imbalance
```

### Daily/session summaries

One compact record per symbol-day:

- Open, high, low, close
- Previous close
- Premarket high and low
- Premarket and regular-session volume
- Gap
- ATR and moving-average primitives
- Session completeness

Simple queries should use this layer instead of scanning minute data.

## Storage Model

Minute bars are symbol-major, immutable, and columnar.

Decoded layout:

```text
open[]
high[]
low[]
close[]
volume[]
```

Each symbol-day uses an implicit minute grid and two distinct bitmaps:

1. `covered_minutes`: the provider or import process authoritatively checked
   the minute.
2. `present_minutes`: at least one valid bar/trade contribution exists and a
   packed bar record follows.

This distinction is required. A covered minute with no trade is real market
information; an uncovered minute is missing data. For a 390-minute regular
session each bitmap is only 49 bytes. A 960-minute extended-hours grid requires
120 bytes per bitmap. Packed bar arrays contain entries only for set presence
bits, addressed with rank/select or a small prefix-count index.

Use fixed-point integer prices, adaptive block compression, and periodic absolute keyframes.

Possible candle encoding:

```text
close_change = close - previous_close
open_change  = open - previous_close
upper_wick   = high - max(open, close)
lower_wick   = min(open, close) - low
```

Volumes and trade counts use unsigned variable-width integers. Optional VWAP
and quality fields use presence bitmaps. Every independently decodable block
has a schema version, fixed-point scale, absolute keyframe, checksum, and
provenance. These compression decisions remain part of the long-term research
store even though the current SQLite row store is sufficient for live
development.

Time is implicit for consecutive bars; gaps are encoded separately.

Slowly changing metadata uses snapshots plus deltas:

- Historical index membership
- ETF holdings
- Sector classifications
- Ticker changes
- Listings and delistings
- Splits and dividends
- Halts
- Calendar exceptions

Acceleration layers:

1. Chunk metadata
2. Daily/session primitives
3. Cached feature bitmaps
4. Sparse event indexes
5. Raw minute paths only after filtering

The planner must always use the lowest-resolution data capable of answering each condition.

## Historical Query Engine

Semantic model:

- Universe
- Anchor
- State
- Feature
- Event
- Condition
- Temporal relation
- Observation unit
- Outcome
- Comparison
- Optional execution rule

Example:

```text
Universe: Historical QQQ members
Anchor: 10:15 ET
Conditions:
- Stock above premarket high
- QQQ above yesterday high
- SMH outperforming QQQ
Question:
- Probability of target before stop
- Future normalized path distribution
```

Execution order:

1. Resolve point-in-time universe membership.
2. Evaluate daily/session conditions.
3. Intersect cached bitmaps.
4. Load minute data only for remaining symbol-days.
5. Load one-second or exact tick windows only when requested.

## Scanner Architecture

### Tier 0: preserve

Record the authoritative stream.

### Tier 1: whole-market cheap state

Maintain compact per-symbol state:

- Last price
- Percent change
- Volume and dollar volume
- Trade velocity
- Spread
- Session high/low
- Staleness
- Basic scanner flags

### Current watchlist trade pressure

The current slice adds an optional transient derived feature for every
subscribed watchlist symbol. Normalized trades, quotes, timestamps, provider
provenance, corrections, cancellations, and data-quality state remain the
authoritative market truth. Pressure is computed from that event stream and is
replayable/configurable; it is not stored as a source fact and must not be
required for canonical market-data persistence.

The feature reports market meaning, not presentation colors:

```text
signed pressure      [-1, +1]
activity/intensity   [0, 1]
buy/sell/unknown trade counts
buy/sell/unknown share volume
last event time
classification method and freshness
```

When a usable quote is available, a trade at/above the ask is classified as
buyer-initiated and a trade at/below the bid as seller-initiated. Locked,
crossed, stale, and inside-spread observations use an explicit fallback or
remain unknown. Without a usable quote, a tick-direction fallback may compare
the trade with the previous distinct trade price; its method remains visible
in the typed result.

Pressure is a time-decayed impulse, not a permanent counter. Separate decaying
buy and sell accumulators produce direction, while gross decaying activity
produces brightness. Half-life, weighting (trade/share/notional), quote
freshness, and saturation are configuration. The UI may map direction and
activity to any pulsing green/red presentation without embedding color policy
in the core.

Recent classified contributions are bounded. Corrections and cancellations
inside that horizon rebuild the transient signal deterministically; older
corrections still repair canonical price and bars but do not attempt to repaint
past UI frames.

### Tier 2: active-symbol processing

Promote top movers, news names, and unusual activity into richer processing.

### Tier 3: detailed microstructure

For watched or traded symbols, calculate:

- Trade-side pressure
- Large prints
- Repeated level tests
- Spread behavior
- Quote-size changes
- Level interaction
- Short-horizon execution signals

Cross-market statistics consume compact lane summaries rather than every raw event.

## Replay System

Replay and live data use the same application event bus:

```text
Live source ──┐
              ├── Market event bus → scanner, charts, UI
Replay source ┘
```

### On-demand replay

When a symbol-day is selected:

1. Check the local cache.
2. Fetch historical Alpaca trades and quotes if missing.
3. Validate and merge events.
4. Compile a compact replay package.
5. Save it locally.
6. Replay from the local package.

Replay package:

```text
exact trades
compressed quote-state stream
seek index
periodic keyframes
minute bars
metadata
```

The renderer may run at 60 or 120 FPS regardless of event frequency.

Quotes can be coalesced into sparse visual microframes, for example at 16.67 ms, storing only changed instruments and useful state. Trades generally remain exact so large prints and timing remain visible.

## One-Second Drill-Down

One-second data bridges minute research and exact ticks.

For selected setups, store:

```text
open
high
low
close
volume
trade count
volume at bid
volume at ask
largest trade
start/end bid and ask
average spread
quote count
```

Workflow:

```text
Minute engine finds setup
        ↓
Load or generate one-second window
        ↓
Identify decisive 2–5 minute section
        ↓
Fetch or load exact trades and quotes
        ↓
Full microstructure replay
```

One minute finds the setup. One second explains it. Exact ticks reconstruct the decisive moment.

## Retention Policy

Do not retain exact full-market SIP quotes forever.

### Permanent, entire market

- One-minute bars
- Daily/session summaries
- Scanner and event indexes
- Optional minute microstructure summaries

### Permanent, important liquid universe

- Exact trades where practical
- Trade-side classification
- One-second summaries for selected events

### Temporary

- Whole-market exact quotes
- Raw WebSocket frames

### Permanent, selected windows

- Exact trades
- Exact quotes
- Corrections
- Status and LULD events

Pin windows around scanner alerts, orders, structural-level tests, top-mover promotion, news-driven activity, and manual markers.

## Live Conditional Decision Support

At entry, define a historical cohort from setup and context. After each completed minute, update the cohort using the observed path:

```text
P(future path | initial setup, observed path so far)
```

Static setup features define the prior family. Dynamic trajectory features increasingly determine similarity:

- Elapsed time
- Return from entry
- MFE and MAE
- Time above/below entry
- VWAP and moving-average distance
- Pullback depth
- Failed pushes
- Higher lows or lower highs
- Volume evolution
- Relative strength
- Market and sector evolution

Outputs:

- Future path density
- Probability target before stop
- Probability stop
- Expected remaining return
- Remaining MFE and MAE
- Effective sample size
- Explanation of why evidence strengthened or weakened

This is evidence for trade management, not an automatic exit rule.

## UI Surfaces

### Live workspace

Scanner, heat map, watchlists, charts, market context, alerts, orders, risk, and recording health.

### Research workspace

Universe, anchor, conditions, question, comparison, results, sample counts, feature contribution, path clouds, and distributions.

### Replay workspace

Historical date, synchronized symbols, scanner reconstruction, tape, quotes, charts, alerts, pause/step, speed, and simulated orders.

### Query construction

Support simple forms, a typed visual node graph, and a purpose-built text DSL. All compile into one canonical typed graph or AST executed by C++.

## Deployment

```text
RAM:
active session, scanner state, current cohorts, hot decoded blocks

Internal SSD:
recent data, caches, current replay library

External NVMe/fast array:
full canonical archive and older replay data

NAS/backup:
raw sources, archive copies, recovery
```

Changing universes changes membership masks; it should not unload and reload terabytes.

Performance profiles should be configurable: portable, balanced, throughput, benchmark, and custom.

## Immediate Build Order

1. Preserve the verified V1 account/order/position and market-data core.
2. Validate persistent watchlist subscription, live IEX streaming, minute-bar
   persistence, reconnect behavior, and visible persistence health with about
   30 paper-trading symbols.
3. Add the deterministic decaying trade-pressure feature as a separate
   consumer of normalized market events. Expose its optional snapshot through
   the existing immutable/incremental data path without adding pressure fields
   to canonical source records.
4. Integrate the pressure snapshot into the existing watchlist UI. No alert
   engine or broad UI rewrite belongs in this slice.
5. Finalize canonical one-minute aggregation and the compressed symbol-day
   chunk contract, including coverage and presence bitmaps.
6. Build resumable Alpaca IEX one-minute backfill for explicitly selected
   research universes and context ETFs.
7. Add the shared bar indicator/feature engine.
8. Add a typed condition AST used by both historical queries and future live
   alerts.
9. Build the first minute-based historical query and outcome pipeline.
10. Add point-in-time universes and metadata only when required by research.
11. Add on-demand exact symbol-day replay fetching and caching when the minute
    research workflow demonstrates a need.

Raw full-feed capture, normalized block pools, SQLite tick sharding, permanent
one-second storage, and SIP performance work are deferred until measured usage
requires them.

## Current Decisions

- One unified platform, not separate scanner and backtester projects.
- One-minute data is the universal searchable history.
- Exact tick data is selective and event-driven.
- Replay is on-demand and locally cached.
- Trades have more long-term value than quotes.
- Quotes are mainly live/replay context and may be compressed or discarded after derivation.
- One-second summaries and raw capture are future optional zoom layers, not
  current dependencies.
- Derived features such as watchlist pressure are transient/recomputable live
  state; they do not redefine or replace canonical market truth and do not
  require permanent per-trade feature history.
- Feed selection is visible once at the application level. Provenance remains
  stored and queryable without repetitive warning language.
- Covered/no-trade minutes and missing minutes are distinct in permanent
  compressed storage.
- Query planning is resolution-aware.
- Universes and metadata must be point-in-time correct.
- Storage layout and cache behavior are first-class design concerns.
- Markdown is the canonical design-document format.
