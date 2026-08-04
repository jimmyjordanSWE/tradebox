# Workstation window model

Trade Box distinguishes window behavior by purpose instead of treating every
surface as an interchangeable floating panel.

## Application chrome

The custom title bar owns a reserved strip at the top of the SDL/DirectX 11
viewport. It contains workstation menus, global connection identity, and native
window controls. It is not part of the saved workspace layout.

No workspace window may move into or behind this strip. Ordinary windows are
clamped to the viewport work rectangle below it.

## Singleton tool windows

Examples: Account, Watchlist, Event Log.

There is at most one instance of each type in a workspace. They are movable,
resizable, closable, reopenable from the `+` menu, and their geometry belongs to
the saved layout.

## Document windows

Examples: symbol charts, combined watchlist charts, order views, and replay
sessions.

Multiple instances may exist. Each needs a stable instance identifier,
configuration payload, and saved geometry. Closing a document removes that
workspace instance only; it does not delete underlying market data.

## System windows

Examples: Settings and Credentials.

These are centered modal windows. They are not dockable, do not participate in
the workspace layout, and block interaction with trading surfaces while open.
Credential secrets are never placed in ImGui layout data or the workspace
database.

## Popovers

Examples: the Menu, Account selector, and `+` creation menu.

Popovers are transient children of application chrome. They have no persistent
geometry and close when their action is completed or focus moves away.

The current floating-workspace implementation uses a normal ImGui window
anchored to the SDL viewport for the custom title bar. The work rectangle is
passed explicitly to the workspace below that bar; it does not depend on
`BeginViewportSideBar()` or other docking-only APIs.

The current layout is persisted in `workspace-layout-v2.ini`. The versioned
filename is intentional: it gives the non-overlapping default arrangement its
own migration boundary while preserving the previous layout file for recovery.
The Menu > Reset window layout action resets registered windows to those
defaults without deleting user data.

## Current window inventory

The current workspace uses stable IDs for singleton tools and stable
symbol-qualified IDs for chart documents. The implemented surfaces are:

- `tool.account`, `tool.watchlist`, `tool.event_log`, `tool.positions`,
  `tool.orders`, and `tool.order_management`;
- `tool.quick_order`, `tool.oco_order`, and `tool.time_sales`;
- one document window per open chart, keyed by its symbol and chart identity.

The `+` menu and table context menus emit presentation actions such as opening
or focusing a surface. They do not perform domain work. The workspace owns
geometry, open state, stable IDs, and layout reset; the application/core owns
the data and command results rendered inside those windows.

Interactive movement and resizing remain native ImGui behavior. The workspace
may apply legal minimum/maximum size constraints, but it does not rewrite the
size selected by the user during a resize interaction.

## Reusable UI primitives

The existing `Workspace::BeginWindow` entry point is the canonical wrapper for
normal workspace windows. The next useful abstractions are presentation-only:

- `WindowSpec`: kind, stable ID, title, default/minimum geometry, and UI
  settings;
- `WindowCommand`: open, close, reset, focus, or create-document actions;
- `ColumnSpec`: stable column ID, default visibility/width, and a renderer for
  a value already present in the snapshot;
- a chart view snapshot containing series, data health, and future indicator
  layers supplied by the application.

These primitives must not become a second domain model. A column renderer may
format a value or emit a command, but it must not calculate a broker decision
or mutate core-owned data. ImGui's built-in hide/reorder column context menu is
the preferred behavior for tables whose complete predefined column set is
available.

## Future lock surface

Lock Workstation will be a full-workspace security surface, not another tool
window. It will hide sensitive content and disable trading actions until local
unlock succeeds while leaving broker and market-data connections under an
explicit policy.
