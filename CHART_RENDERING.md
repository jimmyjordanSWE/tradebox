# Adaptive-resolution chart concept

## Intent

Trade Box should present one continuous chart whose representation becomes more
granular toward the current moment. It is not a conventional chart with one
timeframe selected for the entire viewport.

The right edge is a live market-state head. Moving left through history may
transition through trades or quotes, one-second bars, five-second bars,
one-minute bars, five-minute bars, and daily bars. The exact bands, widths, and
aggregation levels are user-configurable.

An illustrative configuration, not a fixed product decision:

| Visible age | Representation |
| --- | --- |
| Current head | Best bid/ask, last trade, spread and sizes |
| 0–10 seconds | Individual trades and quote changes |
| 10 seconds–2 minutes | One-second bars |
| 2–30 minutes | Five-second bars |
| 30 minutes–1 session | One-minute or five-minute bars |
| Older sessions | Daily bars |

The live head may occupy a fixed or proportional pixel width. A sequence-based
tick band is also possible: its horizontal coordinate represents event order
rather than elapsed time. Such a band must be visually identified because it
is not a linear time scale.

The first slice consumes trades and best bid/ask quotes. When the subscribed
market-data package exposes Level II depth, the same source-neutral event model
can feed it into this region. The chart therefore calls the region the live
market-state head: full depth is an optional input, while the trade/quote
microstructure view remains useful without it.

## One chart, one renderer

An ImGui chart window is a logical clipped region inside the application's one
SDL/OpenGL framebuffer. It is not a separate OpenGL context or native canvas.
The chart emits draw commands into an ImGui draw list, and the OpenGL backend
renders the resulting vertices, indices, textures, and clip rectangles.

The renderer should not shift a framebuffer by one pixel for each update.
Instead, it evaluates a view transform against in-memory data and emits the
visible geometry:

```text
market event time or sequence
        ↓
adaptive horizontal transform
        ↓
screen X

price
        ↓
price-axis transform
        ↓
screen Y
```

As the live clock advances, X coordinates change. At 120 Hz the renderer may
produce 120 visual frames per second, but market events and visual frames remain
separate concerns:

- The feed handler preserves every event.
- The scanner consumes events independently of rendering.
- The chart coalesces all updates received before the next display frame.
- VSync normally caps drawing to the physical monitor refresh rate.

The same renderer accepts either the wall clock or the replay clock. No chart
logic should need to know whether it is live or replaying.

## Multiresolution data pyramid

Each active instrument keeps contiguous hot data for the required levels:

```text
recent trades ring
recent quotes ring
1-second bars
5-second bars
1-minute bars
5-minute bars
daily bars
```

Aggregates are updated incrementally when canonical events arrive. Indicator
values are calculated when their source data changes, not during every rendered
frame. A visible band selects exactly one representation for a time interval so
the same activity is not drawn twice at a resolution boundary.

Historical storage and live aggregation may have different sources, so each
level retains feed and provenance. Corrections update the affected aggregates
and append correction events to the universal replay timeline.

For a very wide series, the renderer should not submit substantially more
samples than there are horizontal pixels. A min/max envelope or equivalent
pixel-aware reduction retains price extremes without drawing invisible points.

## Pan, zoom and level-of-detail

Pan and zoom change a camera/view object, not the underlying data:

```text
ChartView
- right-edge clock
- visible price range
- horizontal zoom
- vertical zoom
- adaptive resolution-band policy
- live-head width
```

Changing the view rebuilds inexpensive screen-space geometry from the relevant
contiguous slices. Cached FreeType glyph textures remain unchanged; only their
screen-space quads move. If profiling later shows that a static layer is
expensive, that layer may be cached in a vertex buffer or framebuffer object
without changing the chart data model.

## Drawing and annotation model

Persistent technical-analysis drawings use market coordinates, never saved
screen pixels:

```text
Drawing
- stable ID
- type
- anchors: timestamp/sequence plus price
- extend-left / extend-right
- style
- source and creation timeline event
```

A ray or infinitely extended line is clipped mathematically to the visible
chart rectangle. Thick lines are rendered as anti-aliased triangle geometry;
OpenGL wide-line behavior is not relied upon.

Supported primitives can grow from:

- Horizontal, vertical and two-anchor trend lines.
- Left/right/both-direction rays.
- Rectangles and measured ranges.
- Arrows and icons.
- Price/time-anchored text.
- Freehand paths.

Freehand input retains raw timestamped samples for replay. A simplified
rendering path may be produced with distance resampling and point reduction.
Optional curve smoothing is appropriate for freehand presentation, but should
not alter analytical price series.

Indicators such as moving averages are contiguous calculated point series and
are normally drawn as anti-aliased polylines. Bézier interpolation should not
be used merely to make a market indicator look smoother because it can imply
values the indicator never produced.

## Rendering layers

A chart frame is composed in a deterministic order:

1. Background and session shading.
2. Grid and axes.
3. Historical candles or aggregate series.
4. Indicators.
5. Live trades, quotes and market-state head.
6. User drawings and selection handles.
7. Price labels, status text and cached vector-derived glyphs.
8. Crosshair and temporary interaction overlays.

All layers are clipped to the chart except intentionally global workspace
overlays. A future whole-workstation pen can use the viewport foreground draw
list and normalized workspace coordinates.

## Performance posture

The initial implementation should use ImGui's dynamic draw lists directly.
Thousands of candles, lines and annotations are inexpensive compared with the
available GPU budget. Optimization should be driven by captured frame timings:

- Keep calculations and source arrays contiguous in memory.
- Update only the current aggregates and affected indicators.
- Cull geometry outside the visible bands.
- Reduce series to useful screen resolution.
- Cache only demonstrably expensive static geometry.
- Keep the market-event pipeline lossless even if the visual frame rate drops.

At 120 Hz the frame budget is approximately 8.33 ms. The application should
measure CPU event processing, chart geometry generation, buffer upload, and GPU
render time separately.

Framebuffer-based smoke tests can render a fixed replay clock and view
configuration, then capture `GL_BACK`. This makes adaptive-chart output
deterministic and testable without capturing the Windows desktop.
