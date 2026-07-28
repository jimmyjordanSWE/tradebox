# Workstation window model

Trade Box distinguishes window behavior by purpose instead of treating every
surface as an interchangeable floating panel.

## Application chrome

The custom title bar owns a reserved strip at the top of the SDL/OpenGL
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

## Future lock surface

Lock Workstation will be a full-workspace security surface, not another tool
window. It will hide sensitive content and disable trading actions until local
unlock succeeds while leaving broker and market-data connections under an
explicit policy.
