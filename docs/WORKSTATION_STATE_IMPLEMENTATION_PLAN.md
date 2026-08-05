# Workstation state implementation plan

> Restoration note (2026-08-05): the workstation state/profile/window
> baseline was recovered after the legacy-GUI cleanup accidentally removed it.
> The visible GUI remains an intentional blank slate. Compiled defaults now
> contain no window or document instances; new windows will be added through
> the common system described in `WINDOW_MODEL.md`.

## Objective

Replace every legacy UI and application-state persistence path with one
profile-backed workstation state system. Each running Trade Box process opens
exactly one `.tbw` profile containing all persistent UI and application state,
from native application placement down to individual tabs, fields, table
columns, checkboxes, chart controls, and dormant order drafts.

This is a big-bang replacement. The application is allowed to remain broken
between demolition and final integration. There will be no compatibility
wrappers, dual writes, or transitional UI persistence paths.

## Persistence boundaries

Trade Box has four distinct persistence authorities:

1. **Workstation profile (`.tbw`)**: UI layout, application settings, window
   instances, documents, watchlists, filters, drafts, and non-secret account
   references.
2. **Windows Credential Manager**: API keys and secrets only.
3. **Operational storage**: broker events, orders, positions, reconciliation,
   command recovery, and other trading records.
4. **Market-data storage**: assets, candles, ticks, coverage, backfill state,
   and market history.

Deleting a workstation profile must not alter credentials, operational data,
or market data.

## 1. Freeze the state contract

Create an exhaustive inventory of every current `App` field and classify it as
one of:

- persistent profile state;
- runtime-only UI state;
- broker/core-owned state;
- derived or cached state;
- secret state.

The inventory becomes a required migration and test-coverage checklist.

Adopt these rules:

- A deliberate user choice expected to survive restart is persistent.
- Button presses, hover state, tooltips, transient validation messages, live
  search results, and in-flight command state are not persistent.
- Broker projections are never copied into profile state.
- Restored order drafts are always dormant and are never submitted
  automatically.
- Persistent identity is never derived from a vector index or visible label.

## 2. Demolish legacy UI persistence

Remove the old implementation before introducing the replacement:

- remove `LoadAppSetting()` and `SaveAppSetting()`;
- remove `LoadWindowPlacement()` and `SaveWindowPlacement()`;
- remove database-backed watchlist persistence;
- move account aliases and last-selected account context out of SQLite;
- remove `LoadWindowVisibility()`, `SaveWindowVisibility()`, and
  `RestoreOpenCharts()`;
- remove `workspace-layout-v7.ini` and all other ImGui disk persistence;
- set `ImGuiIO::IniFilename` to `nullptr`;
- remove index-based order-ticket IDs;
- remove the transient `Workspace::known_windows_` registry;
- move persistent fields out of the large GUI `App` structure;
- delete obsolete UI-setting and database-watchlist tests.

Remove the obsolete operational database tables:

```sql
DROP TABLE IF EXISTS watchlist;
DROP TABLE IF EXISTS window_placement;
DROP TABLE IF EXISTS account_aliases;
DROP TABLE IF EXISTS app_settings;
```

Rename the market database's generic `app_settings` table to a
purpose-specific `market_metadata` table. Retain only market-derived metadata,
such as the asset catalog. Do not delete market history, operational journals,
or credentials.

## 3. Create the workstation-state library

Add an ImGui-independent library:

```text
include/tradebox/workstation/
|-- state.h
|-- defaults.h
|-- validation.h
|-- profile_codec.h
|-- profile_store.h
|-- profile_lock.h
`-- profile_manager.h

src/workstation/
|-- defaults.cpp
|-- validation.cpp
|-- profile_codec.cpp
|-- profile_store.cpp
|-- profile_lock.cpp
`-- profile_manager.cpp
```

Add `toml++` through CMake. Do not create a custom TOML parser.

The state library must not depend on ImGui, SDL, widget labels, broker adapter
types, or market-data storage. Use logical geometry types, UUIDs, and strongly
typed enums.

Principal structures:

```cpp
struct WorkstationState;
struct ApplicationSettings;
struct NativeWindowState;
struct WorkspaceState;
struct WindowInstanceState;
struct ChartDocumentState;
struct OrderTicketState;
struct AccountContext;
struct PersistentTableState;
struct SettingsWindowState;
```

## 4. Define defaults and the profile schema

Provide one compiled default authority:

```cpp
WorkstationState WorkstationState::Defaults();
```

Defaults cover:

- native application geometry;
- an empty initial window/document inventory for the blank-slate rebuild;
- initial watchlist;
- chart defaults;
- table columns;
- quick-order and bracket-order defaults;
- settings-window state;
- no restored live connection;
- no automatically executable order draft.

The profile begins with:

```toml
[profile]
schema_version = 1
id = "<uuid>"
name = "Default"

[application]
# Appearance and performance settings.

[native_window]
# Logical display-aware geometry.

[account_context]
# Non-secret credential reference and intended environment.

[workspace]
# Selection, watchlists, and workspace behavior.

[windows]
# Singleton and document window instances.

[documents]
# Charts, order tickets, and future document types.
```

Serialization must be deterministic: stable keys, stable collection ordering,
normalized numeric values, and no volatile timestamps that make snapshots
noisy.

## 5. Implement profile storage and ownership

Profiles live under:

```text
%LOCALAPPDATA%\TradeBox\workspaces\
```

The default launch target is `Default.tbw`.

Support explicit launch selection:

```text
TradeBoxNative.exe --workspace <path>
TradeBoxNative.exe --workspace <path> --read-only
```

Implement:

- directory scanning for profile discovery;
- an exclusive process-lifetime lock for writable profiles;
- read-only profile loading;
- atomic temporary-file replacement;
- revisioned background autosave;
- debounced field saving;
- save after completed window movement and resizing;
- synchronous shutdown flush;
- corruption reporting without overwriting the source;
- schema validation and explicit migrations;
- clone, import, export, rename, delete, and reset;
- immutable snapshots passed to the writer thread.

If state changes while an older revision is being written, schedule the newer
revision immediately. An older revision must never replace a newer one.

If another process owns the selected profile, offer read-only open, clone and
open, or cancel. Do not merge concurrently edited profiles.

## 6. Replace the window system

Introduce:

```cpp
struct WindowSpec;
struct WindowInstanceState;
class WindowRegistry;
enum class WindowKind;
struct WindowCommand;
```

`WindowSpec` owns static policy:

- type ID;
- title policy;
- window kind;
- default and minimum geometry;
- allowed behavior;
- default content state.

`WindowInstanceState` owns persistent instance state:

- UUID or stable singleton ID;
- open or closed state;
- logical geometry;
- display identity;
- content or document reference;
- focus request or ordering where meaningful.

Use durable IDs:

```text
tool.account
tool.activity
tool.watchlist
chart:{uuid}
order-ticket:{uuid}
replay:{uuid}
```

Closed windows remain registered and persistent. Closing a document and
deleting it are distinct operations.

Store native and child-window geometry in logical, display-relative
coordinates. On restoration, locate the display, apply current DPI scaling,
fall back safely when the display is missing, enforce minimum sizes, and clamp
the surface into the usable workspace.

## 7. Build persistent UI primitives

Create reusable primitives for:

- persistent windows;
- persistent tabs;
- persistent text and numeric fields;
- persistent checkboxes;
- persistent filters;
- persistent collapsible sections;
- persistent tables;
- persistent chart controls;
- persistent document factories.

The table primitive owns semantic column state:

```cpp
struct ColumnState {
    std::string id;
    int order;
    float width;
    bool visible;
    SortDirection sort;
};
```

Column width, order, visibility, and sorting must not depend on an ImGui INI
file. Visible widget labels are presentation only; stable semantic IDs drive
state.

## 8. Rebuild every UI surface

Reconnect every surface directly to `WorkstationState`:

- Account;
- Watchlist;
- Activity and its selected tab;
- Positions table;
- Orders table and filters;
- Events table;
- Order Management;
- Quick Order;
- Bracket Order;
- Time & Sales;
- chart documents;
- chart timeframe, range, visible bars, and visual layers;
- dynamically created order tickets;
- performance and presentation settings;
- native application geometry.

Chart identity is a document UUID plus the provider's stable instrument ID,
not the visible symbol alone.

Order-ticket drafts include intended account and environment context. They are
restored as dormant drafts and revalidated before any submission.

## 9. Generalize credential slots

Replace the fixed paper/live credential API with stable credential slots:

```cpp
struct CredentialSlotId;

struct CredentialReference {
    CredentialSlotId slot;
    BrokerProvider provider;
    AccountEnvironment environment;
};
```

Profiles store only the credential reference and safe display metadata.
Secrets remain in Windows Credential Manager. Existing credentials must not be
erased by the refactor.

Profile-loading safety rules:

- an in-flight broker command blocks in-process profile switching;
- a different account context requires explicit disconnect or switch
  confirmation;
- loading a profile never silently connects to live trading;
- opening the profile in a new process is always available.

## 10. Build Settings and Profile Manager

Create a modal system window organized into:

- Profiles;
- Workspace;
- Appearance;
- Performance;
- Chart defaults;
- Trading UI;
- Credentials;
- Data and storage information;
- Reset and recovery.

Profile operations:

- new from defaults;
- new from current;
- load in this instance;
- open in a new instance;
- save now;
- save as;
- clone;
- rename;
- import;
- export snapshot;
- delete;
- reset.

Profile loading is transactional:

1. Lock and validate the target profile.
2. Refuse unsafe account or command transitions.
3. Flush the current profile.
4. Replace the in-memory state.
5. Rebuild windows and documents.
6. Release the previous profile lock.

A failed load leaves the current profile untouched.

## 11. Restore and verify the application

After every UI surface uses the new state system:

- restore the complete application build;
- remove unused fields, functions, includes, and schema code;
- update `WINDOW_MODEL.md`, `ARCHITECTURE.md`, and related documentation;
- add an architecture test forbidding UI persistence through SQLite or ImGui
  INI;
- add an architecture test preventing secrets and broker projections from
  entering `WorkstationState`.

Required tests:

- compiled defaults;
- deterministic TOML golden snapshot;
- encode/decode round trip;
- validation and clamping;
- schema migration;
- corrupt and truncated file recovery;
- atomic-write failure;
- stale revision ordering;
- exclusive multi-process locking;
- read-only profile behavior;
- clone, import, export, rename, and delete;
- deletion of the profile restoring defaults;
- multi-monitor and DPI restoration;
- window close versus document deletion;
- persistent selected tabs;
- persistence of every checkbox and field;
- table resize, reorder, hide, and sort;
- chart restoration;
- account-context isolation;
- dormant order-draft restoration;
- profile operations leaving operational and market databases unchanged;
- credentials never appearing in a profile;
- two different profiles running simultaneously.

## Completion criteria

The replacement is complete when:

- no UI or application setting is read from or written to SQLite;
- no ImGui INI file exists;
- each process owns exactly one workstation profile;
- two processes can safely run with different profiles;
- the same profile cannot be modified concurrently;
- deleting one `.tbw` restores sane defaults;
- restoring a profile reproduces the complete workstation down to tabs,
  fields, columns, charts, and drafts;
- profile operations cannot modify market data, operational journals, or
  credentials;
- no legacy compatibility persistence remains;
- the complete build and test suite passes.
