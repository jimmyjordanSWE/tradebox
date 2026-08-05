# Luna task: first chart window

This is a temporary implementation guide for the next chart-window task. Read
`AGENTS.md` first. Current code, public types, CMake targets, and tests are the
source of truth. Delete this file when the task is complete.

## Objective

Deliver the first real persistent chart window through the existing TradeBox
systems. The first milestone is intentionally narrow:

- one OHLC candlestick chart;
- optional volume area/pane;
- the current forming bar;
- automatic price scaling over the visible bars;
- an optional crosshair;
- symbol and timeframe presentation;
- explicit loading, empty, missing-history, and error states;
- persistent document settings and window geometry.

This milestone is not an advanced chart engine. Do not add indicators,
drawings, orders, annotations, multiple panes beyond price/volume, renderer
registries, plugin systems, undo/redo, custom DirectX rendering, or TradingView
feature parity.

## Existing systems that must be used

Do not replace or work around these:

- `tradebox::workstation::ChartDocumentState` owns persistent chart choices.
- `tradebox::workstation::WindowInstanceState` owns persistent window state.
- `tradebox::workstation::ProfileStore`, profile codec, defaults, and validation
  own `.tbw` persistence.
- `tradebox::ui::Workspace::BeginWindow` and `EndWindow` own window placement,
  close state, work-area constraints, and dirty tracking.
- `tradebox::application::TradingApplication::SnapshotForUi` is the GUI read
  boundary.
- `tradebox::application::TradingApplication::RequestMarketHistory` is the
  history-request boundary.
- `tradebox::core::BarSeriesSnapshot` is the authoritative bar read model. It
  already distinguishes finalized/revised bars, `current_bar`, latest price,
  missing ranges, and revision.
- `BarStore`, market-data convergence, Alpaca history loading, in-flight range
  deduplication, and SQLite market persistence already own data behavior.

The GUI must never call `Database`, `AlpacaService`, or core stores directly.
It must never aggregate provider/tick data into authoritative bars.

## Ownership for this task

| Concern | Owner |
| --- | --- |
| Bar identity, OHLCV values, live-bar convergence, revisions, missing coverage | Existing core/application data path |
| Coordinating chart requests and returning complete GUI read state | Application layer |
| Persistent symbol/instrument, timeframe, visibility options, and navigation choices | `ChartDocumentState` in the workstation profile |
| Persistent open state and geometry | `WindowInstanceState` through `Workspace` |
| Viewport layout, coordinate transforms, candle/volume drawing, hit testing, crosshair, and current gesture | Chart GUI adapter |
| Market-history durability | Existing market-data SQLite persistence only |

Chart calculations that exist only to map authoritative values to pixels are UI
presentation logic. Trading/domain calculations do not belong in the chart.

## Mandatory design gates

Stop and ask Jimmy before feature code if these questions have not already been
answered. Present the existing code, the gap, and a recommended system-level
extension. Do not solve them locally in `main.cpp`.

### 1. Chart lifecycle and stable identity

The profile has `workspace.charts`, but there is no implemented chart document
factory or general window registry. Establish whether the first chart is:

- a compiled singleton such as `chart.main`; or
- a dynamically created document requiring a shared stable-ID/document factory.

Do not create a feature-local UUID generator, derive identity from symbol/title,
or use a vector index. Define how a chart document maps to its
`WindowInstanceState`, how close differs from delete, and how it is reopened.

### 2. Instrument identity across the UI boundary

`ChartDocumentState` has `instrument_id`, but `UiChartQuery` currently requests
by symbol and timeframe. Decide how the application boundary preserves the
provider's stable instrument identity while retaining symbol as presentation.

Do not silently ignore `instrument_id`, make symbol the durable identity, or
let the GUI resolve broker-specific keys.

### 3. Initial and navigated data range

The profile currently stores `visible_bars`, while `UiChartQuery` requires an
absolute `BarRange`. Decide which established layer converts chart viewport
intent into a requested time range, including the anchor time and behavior for
calendar gaps.

Do not query an unbounded/multi-year range every frame, use a magic date span,
or call `std::chrono::system_clock::now()` throughout rendering as a substitute
for a defined request policy.

### 4. Loading and failure state

`BarSeriesSnapshot` exposes bars and missing ranges but no explicit in-flight or
request-error state, while `RequestMarketHistory` returns `void`. Decide how the
application exposes loading, failure, retry, and disconnected/unavailable state
to every client.

Do not infer “loading” from empty bars forever, hide failures, add GUI timers as
truth, or read broker/UI event internals directly.

Implementation may continue only after these decisions have an approved owner,
typed boundary, lifecycle, and tests.

## Implementation sequence after the gates are resolved

### 1. Protect the baseline

- Preserve the existing uncommitted chrome, font, icon, workspace, and asset
  changes. Do not discard or broadly reformat them.
- Build and run the current relevant tests before changing chart behavior.
- Inspect the complete profile codec and application snapshot path before
  editing them.

### 2. Complete chart document/window integration

- Use the approved stable document identity and lifecycle.
- Extend `ChartDocumentState` only for deliberate persistent user choices.
- Keep hover, active drag, crosshair cursor position, cached pixel geometry,
  and other current-interaction state out of `.tbw`.
- Carry every persistent addition through compiled defaults, validation,
  deterministic encoding/decoding, dirty tracking, and round-trip tests.
- Render through `Workspace::BeginWindow`/`EndWindow`; never call raw
  `ImGui::Begin` for the persistent chart surface.
- Do not add ImGui INI persistence or separate chart/window files.

### 3. Introduce a focused chart GUI component

Move chart rendering and interaction out of `main.cpp`. Add the smallest
cohesive GUI-side component needed for this milestone, using the repository's
lowercase path/naming conventions. It may depend on ImGui and immutable
application/core read types. Core and application must not depend on it.

Separate deterministic geometry from ImGui submission so it can be unit tested:

- visible-index selection;
- time/index-to-X transform;
- price-to-Y and Y-to-price transform;
- visible-price autoscale with finite padding;
- candle body/wick geometry;
- volume normalization;
- crosshair hit/label values.

Use `core::Decimal` values as authoritative inputs and convert to display
coordinates only at the presentation boundary. Handle empty, single-value,
zero-range, extreme, and invalid-size viewports without division by zero or
non-finite geometry.

Do not introduce a renderer registry, generic property bag, chart service,
chart database, or custom graphics backend for this milestone.

### 4. Use one application snapshot per frame

- Build the chart portion of `UiSnapshotQuery` from open chart documents using
  the approved identity/range contract.
- Call `SnapshotForUi` once per frame when the application is available.
- Match returned chart state to documents through the approved stable identity,
  not accidental vector ordering unless ordering is explicitly part of the
  typed contract.
- Treat snapshots as immutable for the full render pass.
- Render a clear local-data-loading state while `TradingApplication` is not yet
  available; never block the first frame on database startup.
- Do not copy or re-sort the entire series per frame. The snapshot is already
  time ordered; restrict work to the visible range.

### 5. Request history through the application

- Request only the approved range through `TradingApplication`.
- Respect existing missing-range and in-flight deduplication behavior.
- Never request history every render frame.
- Make missing local coverage visible and provide retry/fetch behavior only as
  defined by the approved application contract.
- Do not write market data from the GUI. Successful fetches already persist
  through the existing market-data system.

### 6. Render the first chart

Within the workspace-managed window:

- reserve explicit rectangles for the chart body, price axis, time axis, and
  optional volume area;
- clip all series drawing to its plot rectangle;
- render a subdued background/grid, candle wicks and bodies, axes, current
  price marker, and optional crosshair;
- render finalized/revised `bars` plus `current_bar` without duplicating the
  same timestamp;
- calculate autoscale only from visible OHLC values;
- preserve distinguishable up/down/unchanged candles without encoding domain
  meaning in the renderer;
- show symbol, timeframe, connection/data status, and missing coverage without
  manufacturing broker state;
- keep all ImGui IDs stable and independent of visible labels.

UI choices that change persistent chart fields must use the existing profile
dirty path. Continuous gestures should not serialize every frame; save the
settled semantic result through the existing debounced profile store.

### 7. Tests and enforced boundaries

Add focused tests for:

- chart document default/validation/codec round trip;
- chart/window identity and close-versus-delete behavior;
- viewport transforms and inverse transforms;
- autoscale for empty, flat, positive, and negative ranges;
- visible-range selection and candle geometry;
- current-bar replacement rather than duplication;
- loading, missing-history, error, and unavailable application states;
- one snapshot request per frame/query assembly behavior where practical;
- persistence only for semantic state, not hover/crosshair/drag state.

Extend architecture checks if needed to prevent chart GUI code from including
database or broker headers and to keep ImGui out of core/application.

Run the smallest chart/workstation tests first, then build the GUI and run the
complete non-credentialed CTest suite. Do not run the Alpaca Paper contract test
without explicit authorization.

## Acceptance criteria

The milestone is complete only when:

- a chart opens/reopens through the approved persistent document/window system;
- restart restores its approved semantic settings and geometry;
- bars reach it only through one immutable application UI snapshot;
- finalized bars and the current bar render correctly without duplication;
- price scaling, candle geometry, volume, resize, and crosshair are stable;
- loading, empty, missing, disconnected, and failed states are explicit;
- no GUI code accesses Alpaca, SQLite, or core stores directly;
- no second state or persistence authority was introduced;
- no full-history work or profile write occurs every frame;
- strict-warning builds and the full non-credentialed test suite pass.

If an acceptance criterion requires a system the repository does not yet have,
stop and discuss that system with Jimmy. Do not mark the milestone complete with
a chart-specific workaround.
