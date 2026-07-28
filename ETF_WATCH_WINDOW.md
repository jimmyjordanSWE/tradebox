# Daily ETF watch window

## Purpose

This is a saved in-app watchlist that will ultimately render all of its members
inside one chart window. It is a market-context instrument, not a collection of
independent chart windows.

The first implementation should establish the watchlist and one-chart rendering
model. Normalization, ratios, spreads, and other interpretation modes are later
layers built on the same underlying time-series data.

## Watchlist

| Symbol | Role |
| --- | --- |
| SPY | Broad market |
| QQQ | Growth and mega-cap technology |
| IWM | Small-cap breadth and risk appetite |
| IGV | Software and software services |
| SMH | Semiconductors and AI infrastructure |
| XLF | Financials |
| XLI | Industrials |
| XLY | Consumer discretionary |
| XLP | Consumer staples and defense |
| XLV | Healthcare and defense |
| XLE | Energy and oil sensitivity |
| TLT | Interest rates and long-duration growth pressure |

## Core comparisons

- Technology: IGV versus SMH.
- Market: QQQ versus SPY and IWM.

## Chart modes

### Raw price

Draw each selected instrument as a simple line using its actual price. This is
useful only where price scales are naturally comparable.

### Normalized performance

Rebase every visible series to the same value at the selected comparison
origin, normally `100`.

```text
normalized(symbol, t) = 100 * price(symbol, t) / price(symbol, origin)
```

This shows which instrument strengthened or weakened over the selected period.
The origin must be explicit and must move predictably when the timeframe or
visible range changes.

### Relative-strength ratio

Plot one instrument divided by another:

```text
relative_strength(A, B, t) = price(A, t) / price(B, t)
```

An increasing `IGV / SMH` line means software is strengthening relative to
semiconductors. A decreasing line means semiconductors are strengthening
relative to software. This is not the same as overlaying two normalized price
series.

Ratio series should also support rebasing to `100` when the user cares about
percentage movement rather than the ratio's absolute value.

## Timeframes

The same watch window should eventually operate on daily, intraday bar, and
live/replay data. All component series must use a common time grid. Missing
observations must be represented as missing; the calculation layer must not
silently invent trades or carry prices across inappropriate session
boundaries.

## Saved-window model

The eventual saved workspace object needs:

- Stable watchlist identifier and display name.
- Ordered symbol membership and descriptive roles.
- Visible/hidden state per series.
- Chart mode: raw, normalized, or relative strength.
- Timeframe and session policy.
- Comparison origin policy.
- Numerator and denominator for ratio mode.
- Per-series color and line style.
- Window placement and size through the normal workspace layout.

The saved object should store configuration, not derived bars. Derived series
are rebuilt deterministically from the underlying time-series cache so the same
window works in live mode and replay.

## Initial user story

From the title-bar `+` menu, the user can eventually open **Daily ETF
Watchlist**. One window appears containing the saved ETF list and one combined
chart. The default view is normalized performance; the user can switch to raw
price or choose a numerator and denominator for a relative-strength chart.

This design is intentionally parked until the current menu, account selection,
login, and credential flow are established.
