# Workstation window baseline

## Current status

The application intentionally starts as a blank native window. There are no
menus, tool windows, document windows, settings windows, popovers, controls, or
overlays registered by default.

This blank surface is not an absence of architecture. The reusable state,
profile, and window infrastructure is present so every new surface can be
built through one consistent system.

## Organization

The UI is organized as composition rather than a master window class with a
deep inheritance tree:

1. `WorkstationState` is the complete persistent state tree.
2. `WorkspaceState` owns window instances, documents, drafts, selections, and
   future table state.
3. `WindowInstanceState` gives every persistent window a stable ID, kind,
   title, open state, logical geometry, selected tab, and table states.
4. `Workspace` is the canonical ImGui adapter. It applies persisted geometry
   before `ImGui::Begin`, then captures geometry and open state after drawing.
5. A future window registry/spec layer will describe static policy and defaults
   without putting domain logic into the window adapter.

This provides one common window mechanism without forcing charts, settings,
order tickets, and tables to inherit behavior they do not share.

## Persistence authority

The active `.tbw` profile is the only authority for UI and application state.
Profiles live by default under:

```text
%LOCALAPPDATA%\TradeBox\workspaces\
```

The application supports:

```text
TradeBoxNative.exe --workspace <path>
TradeBoxNative.exe --workspace <path> --read-only
```

The profile store provides deterministic TOML encoding, validation, exclusive
writable-profile locking, debounced saving, atomic replacement, and synchronous
shutdown flush. Dear ImGui INI persistence is disabled.

The `.lock` sidecar exists only while a writable profile is open. It contains
no application state.

Deleting the `.tbw` file resets the program to compiled defaults. It does not
delete credentials, operational trading records, or market data.

## Persistence boundaries

- `.tbw` profile: native window placement, application settings, UI layout,
  windows, tabs, fields, checkboxes, tables, documents, and dormant drafts.
- Windows Credential Manager: API keys and secrets.
- Operational database: broker events, command journal, reconciliation,
  positions, and orders.
- Market-data database: assets, ticks, candles, coverage, and market history.

The profile stores only a non-secret credential-slot reference and intended
account context.

## Blank defaults

`WorkstationState::Defaults()` creates a valid profile identity and sane
application/native-window defaults. Its window, chart-document, and
order-ticket collections are empty.

Old window IDs and layouts are not registered or rendered by the blank shell.
New defaults will be added only when their corresponding new windows are
designed.

## Contract for adding a window

Every new persistent window must follow the same sequence:

1. Assign a stable type ID and, for documents, a stable instance ID.
2. Define its semantic persistent fields in the workstation state model.
3. Define sane compiled defaults and validation limits.
4. Serialize the fields through the deterministic profile codec.
5. Render the window through `Workspace::BeginWindow` and
   `Workspace::EndWindow`.
6. Keep broker/core projections and transient interaction state outside the
   profile.
7. Add default, validation, and encode/decode round-trip tests.

Visible labels, vector indexes, and current symbols are not acceptable window
identities.

## Known next primitive

`PersistentTableState` and `ColumnState` are already part of the profile
schema. Before the first configurable table is introduced, a semantic table
adapter must bind stable column IDs, widths, order, visibility, and sort state
to those structures. ImGui's private INI data must not become a second state
authority.
