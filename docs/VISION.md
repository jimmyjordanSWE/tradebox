# Trade Box vision

Trade Box is one native, replayable trading workstation. Alpaca is the first
broker and data provider, but the application owns the workspace, journal,
recordings, annotations, watchlists, tags, search indexes, and replay clock.
News providers, briefing archives, audio, LLM analysis, and future broker
connections are sources feeding the same workstation rather than separate
features with separate notions of time.

The defining idea is that a trading session is a recorded event stream. Market
events, broker events, news, briefings, workspace actions, chart interactions,
journal markers, tags, model outputs, and audio notes share one timeline. A live
session and a replay session drive the same in-memory models; replay merely
replaces wall-clock time with a controllable clock. This enables pausing,
stepping, changing speed, skipping inactivity, and reviewing an entire trading
day in minutes without changing how charts are rendered.

## Non-negotiable timeline invariant

Every source event distinguishes at least three times:

- `event_time`: when the underlying event happened or was published.
- `available_at`: the earliest time this workstation could legitimately know it.
- `recorded_at`: when this installation persisted it.

Replay is ordered by `available_at`, with the original event time retained for
charts and analysis. This prevents look-ahead: a corrected article, late market
print, revised briefing, transcript, or LLM summary must never appear earlier in
replay than it could have appeared live.

Every event also retains source, source-specific identity, type, symbol when
applicable, schema version, and original payload. Derived events such as an LLM
summary retain their model/prompt provenance and references to their inputs.
Corrections append new events; they do not rewrite what the workstation
previously knew.

## V1 foundation

V1 establishes the broker-connected core:

- Save Alpaca paper/live API credentials in Windows Credential Manager.
- Maintain reconciled account, order, and position state from REST snapshots
  and the account WebSocket.
- Submit, replace, cancel, and emergency-close broker-native orders through
  typed commands, durable intent journaling, idempotency, and safety gates.
- Maintain a persistent watchlist/workspace and stable instrument identities.
- Backfill, cache, and live-update candlesticks for every supported timeframe.
- Share supervised market-data and account WebSockets across consumers.
- Append raw market and account events to source-neutral SQLite timelines.

Scanner, audio capture, transcription, news connectors, automated strategy
execution, and the replay UI remain outside V1. The universal timeline exists
now so those features do not require retrofitting recordability later.

## Runtime model

The render thread owns the visible chart arrays. Network workers parse Alpaca
messages and push compact events through a queue. At the beginning of each
frame, the render thread drains that queue and updates contiguous in-memory bar
arrays. SQLite is never consulted by the render loop.

Historical data is read from SQLite only when a symbol opens, then refreshed
asynchronously from Alpaca. Live events update RAM immediately and are queued
to a batched database writer independently. Every future source must use the
same live/replay interface: live adapters publish events as they arrive, while a
replay adapter publishes recorded events as the replay clock crosses their
`available_at` time.

## Search and tags

The immutable timeline is the evidence layer. Searchable tables and indexes are
rebuildable projections over that evidence. Tags are versioned assertions, not
columns burned into the original event:

- manual tags, such as `feeling:uneasy` or `setup:gap_and_go`;
- factual tags, such as `news:present`, `direction:short`, or `result:stopped`;
- model-derived tags with model version, confidence, and input provenance;
- relationships connecting a trade, news items, annotations, and journal notes.

This supports questions such as “short trades with positive news,” “trades with
no known news at entry time,” or “days containing this setup.” Search results
can become replay playlists: a query selects sessions or time ranges, and the
same replay engine runs each result with a chosen speed and inactivity-skipping
policy.

Application data lives in `%LOCALAPPDATA%\TradeBox`, outside the source tree and
outside the OneDrive-synchronized repository.

## Later, without commitment to schedule

- A combined ETF watch window may be designed after the empty GUI shell is
  re-established.
- A replay clock reads the recorded event stream into the same event queue.
- Semantic UI actions and broker updates join the universal timeline.
- Journal markers and raw audio clips attach to timeline timestamps.
- Speech-to-text and behavioral analysis operate on journal data.
- Automated strategy orders are introduced only after their own risk,
  supervision, and paper-account acceptance criteria are designed and tested.
