# Luna handoff: usable trading workstation UI

## Objective

Turn the existing ImGui shell into a fast live-trading workspace built around
one action loop:

1. See roughly 30 symbols at once in a fixed 1-minute **CHARTS** view.
2. Scan those symbols in a spreadsheet-like market grid.
3. Click a chart or grid row to select a symbol.
4. Have the order ticket immediately show that symbol's saved draft.
5. Edit the draft, prepare an order, and submit only through the existing
   headless order-command path.

The first milestone is a usable paper-trading UI. It is not a strategy engine,
indicator editor, alarm engine, or portfolio/risk system.

## Current code constraints

- `src/adapters/gui/main.cpp` owns nearly all visible UI state and drawing.
- `include/tradebox/ui/model.h` contains the testable UI validation/state layer.
- The watchlist is already persisted through `Database::LoadWatchlist` and
  `SaveWatchlist`.
- Live symbol projections are available through `MarketDataSnapshot`.
- Existing chart data is delivered through `HistoricalBars`/`DailyBar` UI events.
- Existing order submission must continue through
  `TradingApplication::SubmitOrder` and `SubmitUiCommand`.
- ImPlot is not currently linked in `CMakeLists.txt`; add it as a pinned
  dependency before using `ImPlot::BeginPlot`.
- Do not move broker/account/order semantics into ImGui drawing code.

## Milestone 1: Charts workspace

Create one singleton window named `CHARTS` (or `CHARTS###charts-workspace`). It
should contain:

- a compact toolbar with the current watchlist count and connection state;
- a 1-minute timeframe label/control, initially fixed to `1Min`;
- a responsive grid of chart tiles, targeting 5 columns on a normal desktop;
- one tile per watchlist symbol, capped at the first 30 symbols for now;
- a symbol label, last price, change versus previous close, and stale/data state;
- a click target covering the whole tile;
- selection highlighting for the active symbol;
- mouse wheel zoom/pan behavior only if it remains usable at tile size.

Clicking a tile must call one shared symbol-selection function. The same
function must be used by the ticker grid and the existing watchlist rows.

The tile renderer may initially use the existing draw-list renderer. Once ImPlot
is wired, use a small `ChartView` adapter so the data model is independent of
ImPlot. Do not create 30 independent top-level ImGui windows.

### 1-minute data contract

Every chart tile must distinguish these states:

- loading;
- live and current;
- cached historical data only;
- stale/disconnected;
- no data.

Never render zero as a substitute for missing price data. The tile should use
the existing provisional minute bars when available and append/update the
historical 1-minute series without mutating data owned by the market-data
thread.

## Milestone 2: market grid

Add a singleton `MARKET` window. This is a scanner/table, not another chart
window. Rows are the watchlist symbols. Start with hard-coded columns and make
the table horizontally scrollable:

- Symbol;
- Last;
- Change % versus previous close;
- Bid;
- Ask;
- Spread;
- 1-minute volume or trade count when available;
- data/stream status.

Use `ImGui::BeginTable` with frozen headers and `ImGuiListClipper`. Add a small
`+` button in the header as a visual affordance, but leave column editing for a
later milestone. The first version must not pretend to support user-defined
columns.

If a value is not backed by an existing reliable source, show `--` and a
tooltip explaining why. In particular, do not calculate “change from open”
until a session-open reference is explicitly available in the data model.

Clicking a row selects the symbol and focuses the order ticket. A context menu
may expose `Open chart tile`, `Focus order ticket`, and `Open time & sales`.

## Milestone 3: persistent symbol selection and order drafts

Replace the current “one order ticket per ad-hoc window” behavior with a
symbol-keyed draft store in the UI model. The minimum shape is:

```cpp
struct OcoDraft {
    bool enabled = false;
    double take_profit_percent = 1.0;
    double stop_loss_percent = 0.5;
    std::string take_profit_price;
    std::string stop_loss_price;
};

struct SymbolOrderDraft {
    OrderEntryDraft simple;
    OcoDraft oco_long;
    OcoDraft oco_short;
    std::string selected_mode = "Simple";
};
```

Store drafts in `std::unordered_map<std::string, SymbolOrderDraft>` keyed by
normalized ticker. `SelectSymbol(symbol)` must:

- save any currently edited widget buffers into the current symbol draft;
- change the active symbol;
- load the destination symbol's draft into the widget buffers;
- update the active chart, market row, and time-and-sales selection;
- keep the order window open and focused.

For the first implementation, “persisted” means preserved while switching
between symbols during the running session. Add SQLite persistence only after
the interaction is stable; use a versioned serialized record and never store
credentials in it.

The order window should have three modes/tabs:

- `Simple`: existing market/limit/stop/stop-limit form;
- `OCO Long`: take-profit and stop-loss exit form for a long position;
- `OCO Short`: take-profit and stop-loss exit form for a short position.

The OCO tabs use defaults of +1.0% take profit and -0.5% stop loss from a
clearly labeled reference price. Use numeric sliders/inputs first. A chart-line
drag interaction and “select level from indicator” are explicitly future work.

The UI must build a typed request and validate it before submission. Extend the
existing order model only where necessary; `NativeOrderRequest` already has
`OrderClass::Oco`, `TakeProfit`, and `StopLoss`. Do not silently submit an OCO
as two unrelated simple orders.

## Window organization

Keep these as separate singleton windows:

- `CHARTS`: 30 one-minute chart tiles;
- `MARKET`: scanner table;
- `ORDER`: one active symbol order ticket;
- `POSITIONS`: existing account positions;
- `ORDERS`: existing broker orders;
- `ACCOUNT`: existing account state;
- `TIME & SALES`: existing selected-symbol tape.

The user should not need to open one order window per ticker. There is one
active order window whose draft changes with symbol selection.

Alarms are a later singleton window. Do not implement alarm persistence or
triggering in this milestone.

## Assignment order

### LUNA-UI-01 — ImPlot and chart adapter

- Add pinned ImPlot sources to CMake.
- Add a small chart-view/data adapter rather than including ImPlot throughout
  application code.
- Prove one 1-minute chart renders from a copied immutable series.
- Preserve the existing capture/smoke-test path.

Acceptance: the application builds, one symbol renders a 1-minute chart, and
no market-data ownership is violated.

### LUNA-UI-02 — shared symbol selection

- Add `SelectSymbol(App&, std::string_view)`.
- Route Short tiles, market rows, watchlist rows, and time-and-sales through it.
- Add model tests for normalization and selection-state transitions where
  possible without SDL.

Acceptance: clicking MSFT after editing AMD changes the active symbol and later
returning to AMD restores the AMD draft.

### LUNA-UI-03 — Charts workspace

- Build the 30-tile responsive layout.
- Subscribe/request only the visible Charts symbols and the active ticket
  symbol, while retaining the existing required-symbol safety behavior.
- Show explicit loading/stale/no-data states.

Acceptance: a 30-symbol watchlist is scannable in one window and clicking any
tile selects that symbol in under one interaction.

### LUNA-UI-04 — market grid

- Add the scanner table with the hard-coded columns above.
- Freeze the header, clip rows, and add row selection/context menu.
- Keep unsupported metrics visibly unavailable.

Acceptance: the table remains usable with 30 symbols and live updates do not
reset scroll position or selection.

### LUNA-UI-05 — order draft store and OCO tabs

- Introduce symbol-keyed drafts.
- Make the single order window follow the active symbol.
- Add Simple/OCO Long/OCO Short tabs and percent defaults.
- Add typed OCO request construction and unit tests for long/short defaults,
  validation, and missing reference prices.

Acceptance: switching symbols preserves independent drafts; no order is sent
when validation fails; all sends still use the existing command journal and
generation/account safety checks.

## Explicit non-goals for this pass

- indicator calculations or indicator-level stop selection;
- right-click chart-to-order level binding;
- editable scanner-column configuration;
- alarm engine;
- automated strategies;
- multi-account UI;
- durable order-draft SQLite schema;
- changes to broker reconciliation or account authority.

## Suggested Lunar prompt

> Read `docs/LUNA_UI_WORKSTATION_HANDOFF.md` completely. Implement only
> `LUNA-UI-01` first. Keep the existing build green, add focused tests where
> practical, and report the files changed, the exact ImPlot pin, and the
> framebuffer/smoke-test result before starting the next assignment.
