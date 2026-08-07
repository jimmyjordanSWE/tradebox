# TradeBox 1.0 — Plan & Progress

## Definition of Done

A desktop application that connects to Alpaca, displays real-time market data on charts and watch lists, shows current positions and orders, and allows placing/canceling/replacing orders with bracket support, all while persisting workspace state across restarts.

---

## Phase 1: Order Entry (Critical)

**Goal**: Users can place, cancel, and replace orders from within the application.

- [x] **1.1 Order ticket window GUI**
  - New: `src/adapters/gui/order_ticket_window.cpp` / `.h`
  - Render `OrderTicketState` fields: symbol, side, type, qty/notional, limit/stop price, TIF, extended hours
  - Wire submit button → `TradingApplication::SubmitOrder()`
  - Show order status/result feedback
  - Register in `main.cpp` alongside other window renderers

- [x] **1.2 Bracket order UI**
  - Render `BracketDraftState` (target %, stop %, GTC, short entry)
  - Attach bracket params to the order before submission

- [ ] **1.3 Order preview/confirmation**
  - Confirmation popup showing order details before sending to broker
  - Live trading confirmation gate (reuse `AccountPopupState.live_trading_confirmed`)

- [ ] **1.4 Order history/status display**
  - Enhance `OrdersWindow` with fill details, status transitions, cancel/replace actions
  - Wire cancel/replace buttons to `TradingApplication::SubmitOrder()`

---

## Phase 2: Safety & Error Handling

**Goal**: Users understand connection state, errors, and recovery status.

- [ ] **2.1 Graceful reconnection UI**
  - Show reconnection status, backoff progress, retry count in status bar

- [ ] **2.2 Market data gap detection**
  - Surface gap info to user when market stream disconnects/reconnects

- [ ] **2.3 Order failure recovery UI**
  - Show recovery state, retry options for failed orders

- [ ] **2.4 Persistence health warnings**
  - Surface `DatabaseWriterTelemetry` (dropped events, write failures) in status bar

- [ ] **2.5 Rate limit awareness**
  - Show remaining API call budget from `RestTransportHealth`

---

## Phase 3: Remaining GUI Windows

**Goal**: All state models have corresponding GUI renderers.

- [ ] **3.1 Time & Sales window**
  - New: `src/adapters/gui/time_sales_window.cpp` / `.h`
  - Render trade tape for `workspace.time_sales_symbol`
  - Use `MarketDataDelta` for live updates

- [ ] **3.2 Instrument Link Groups UI**
  - Create/edit/delete groups, assign colors, select instruments
  - Wire to chart window for linked navigation

- [ ] **3.3 Indicator Suites manager**
  - Save/load indicator presets, apply to charts

- [ ] **3.4 Chart Drawings**
  - Horizontal/vertical lines, trend lines, rectangles
  - Persist via `ChartDrawingState` (already exists)

---

## Phase 4: Packaging & Distribution

**Goal**: Users can install and run TradeBox without building from source.

- [ ] **4.1 CMake install target**
  - Bundle .exe, assets, fonts, licenses into a directory

- [ ] **4.2 Windows installer**
  - NSIS or WiX installer with Start Menu shortcut

- [ ] **4.3 CI/CD pipeline**
  - GitHub Actions: build, run tests, create release artifact

- [ ] **4.4 Auto-update mechanism**
  - Simple version check + download

- [ ] **4.5 Portable mode**
  - Already partially supported via `--workspace` flag

---

## Phase 5: Testing & Hardening

**Goal**: Confidence that the application is stable and correct.

- [ ] **5.1 Order entry integration tests**
  - Test full order lifecycle through `OrderExecutionService`

- [ ] **5.2 Soak test**
  - Run with synthetic market data for 24h, verify no memory leaks or crashes

- [ ] **5.3 Multi-monitor testing**
  - Verify window positioning, DPI scaling

- [ ] **5.4 Credential edge cases**
  - Test save/load/rename/delete with special characters, long names

- [ ] **5.5 Profile migration test**
  - Verify v2→v3 schema upgrade works

- [ ] **5.6 Clean up `main.cpp`**
  - Extract `ApplicationHost` class (1157 lines is too large)

---

## Progress Log

| Date | Phase | Item | Status |
|------|-------|------|--------|
| 2026-08-07 | — | Initial commit of current state | ✅ |
| 2026-08-07 | — | Architecture analysis complete | ✅ |
| 2026-08-07 | — | 1.0 roadmap created | ✅ |
| 2026-08-07 | — | PLAN.md created | ✅ |
| 2026-08-07 | 1.1 | Order ticket window GUI (header, impl, CMake, main.cpp registration) | ✅ |
| 2026-08-07 | — | Committed: phase-1.1 order ticket window | ✅ |