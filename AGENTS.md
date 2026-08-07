# TradeBox development rules for AI agents

This repository is a system, not a collection of independently completed
features. Optimize for architectural integrity, consistency, and explicit
ownership before local task completion.

## Prime directive

Use the repository's established systems for every feature that falls within
their responsibility. Do not create a local substitute, parallel path, special
case, compatibility shim, or temporary workaround merely to finish the current
task.

If the requested behavior cannot be implemented cleanly through an existing
system, stop before implementing it. Explain:

1. the requested behavior;
2. the existing systems and extension points you inspected;
3. the exact capability that is missing;
4. why a local solution would bypass or duplicate an authority; and
5. the architectural decision or system extension that is needed.

Then ask the user how to proceed. A task being blocked by a missing system is a
valid and preferred outcome. Do not silently invent the missing architecture.

## Required workflow before editing

For every change:

1. Inspect the owning implementation, public types, tests, target definitions,
   and enforced architecture checks.
2. Search the repository for existing types, registries, adapters, services,
   state models, commands, codecs, validation, and persistence mechanisms
   related to the request.
3. Identify the single authoritative owner of each piece of state and behavior.
4. State which existing system and extension point the change will use.
5. Implement through that extension point and add tests at the owning layer.
6. Add or update executable tests when behavior or ownership changes.

Do not begin feature implementation based only on the task description or on
the nearest source file. Existing architecture must be discovered first.

## What counts as an architectural gap

Stop and ask the user before coding when the work would require any of these:

- a new source of truth, state store, registry, manager, service, lifecycle, or
  persistence path;
- a new cross-layer communication mechanism or dependency direction;
- state ownership that is absent, ambiguous, or split between components;
- bypassing a typed command, immutable snapshot, adapter, validator, codec, or
  profile mechanism;
- a one-off implementation of behavior that future windows or features would
  also need;
- extending an existing system beyond the responsibility established by its
  types, dependencies, callers, and tests;
- implementing behavior described only as a future proposal;
- contradicting an architecture test or established system;
- retaining a workaround with the intention of integrating it properly later.

A straightforward addition through an already-defined extension point is not
an architectural gap. For example, adding a new value to an established typed
model and carrying it through that model's existing validation, codec, and
tests is a normal extension. Creating a second place to store that value is an
architectural gap.

If unsure whether an extension point is established, treat that uncertainty as
a gap and ask.

## Layer and authority rules

Not everything belongs in `core`. Put behavior in the layer that owns its
semantics:

- `core`: transport- and UI-independent trading semantics, deterministic state
  transitions, safety state, and market-data projections;
- `application`: use-case orchestration, typed commands, and immutable read
  models shared by clients;
- `broker` adapters: external broker protocol translation and validation;
- `persistence`: operational and market-data storage behind defined
  interfaces;
- `workstation`: ImGui-independent persistent workstation/profile state,
  defaults, validation, encoding, locking, and storage;
- GUI adapter: rendering, input translation, and ephemeral interaction state.

The GUI must not call Alpaca directly, query SQLite while rendering, reproduce
domain validation, infer broker truth, aggregate market data, or create a
second persistence/state authority. It renders immutable application snapshots
and emits typed commands.

Remote or persisted input is validated at its boundary. Errors and unsupported
states must remain explicit; do not silently coerce, ignore, or hide them.

### Mandatory placement test

Classify every new behavior and every new field before writing code:

| Concern | Owner | Must not own |
| --- | --- | --- |
| Domain invariants, trading calculations, deterministic reducers, reconciliation, safety rules, broker-independent command/state semantics | `core` | UI preferences, rendering, operating-system APIs, transport formats, SQL, files, credentials |
| Coordination of a user/system use case across core and ports, command submission, assembling client read models | `application` | Widget behavior, broker wire formats, database implementation details |
| HTTP/WebSocket/broker payloads, authentication protocol, remote error translation | broker adapter | Domain authority, UI state, direct feature-specific persistence policy |
| Durable operational and market-data storage implementation | `persistence` adapter | Domain decisions, presentation state, secrets |
| Persistent user choices and workstation/window/document configuration | `workstation` profile system | Broker truth, live projections, secrets, operational history |
| Pixels, widgets, input gestures, presentation formatting, and truly ephemeral interaction state | GUI adapter | Domain rules, durable state, direct broker/database access, background business workflows |
| Secret material | platform credential store | Profiles, SQLite, logs, snapshots, source files |

Use these tests in order:

1. Would the rule still have to be true with no GUI and a different broker?
   It is probably core domain semantics.
2. Does it coordinate a use case or translate core state for every client? It
   is probably application behavior.
3. Is it specifically about an external protocol, storage engine, operating
   system, or rendering library? It belongs in the corresponding adapter.
4. Is it an intentional user choice that must survive restart? It belongs in
   the workstation state/profile system unless it is a secret.
5. Is it only needed to render or manage the current gesture/frame? It belongs
   in the GUI and must remain ephemeral.
6. Does none of the above provide a clean owner? Stop. The repository may need
   a new system; do not use `core`, the GUI, or a database as a miscellaneous
   holding area.

File location alone does not establish ownership. A type belongs to the layer
whose semantics and lifecycle it represents. Do not put code in `core` merely
to make it reusable, and do not put code in the GUI merely because the first
consumer is a window.

### Rules for a possibly missing system

When a feature introduces a coherent responsibility not owned above, do not
force it into the nearest layer. Stop and present a system-boundary proposal
for discussion. At minimum identify:

- the responsibility and why existing owners cannot correctly own it;
- authoritative state versus derived/cache state;
- public inputs, outputs, commands, events, and errors;
- lifecycle, concurrency, and failure behavior;
- persistence needs and deletion/retention semantics;
- allowed dependency directions; and
- contract and architecture tests that would enforce the boundary.

Do not create the directory, manager, service, schema, or generic abstraction
until the user approves that boundary.

## Persistence placement and database rules

Persistence is a semantic decision, not an implementation convenience. A
value must have one durable authority only.

Use the existing authorities as follows:

| Data | Durable authority |
| --- | --- |
| Window geometry, open state, UI layout, tabs, fields, filters, table configuration, watchlists, application settings, document state, and dormant drafts | Active `.tbw` workstation profile |
| API keys, secrets, and other credentials | Windows Credential Manager through the platform credential system |
| Broker events, command intent/recovery journal, reconciliation/audit records, orders, positions, and account activity | Operational SQLite database through the persistence system |
| Stable asset metadata, raw market events, ticks, candles, coverage, backfill state, and market history | Market-data SQLite database through the persistence system |
| Live broker projections and client read models | Runtime state owned by core/application; persist only the underlying records through an already-defined journal/store |
| Ephemeral UI interaction such as hover, active drag, open tooltip, transient error display, and in-flight gesture state | Memory only in the GUI adapter |

Before adding any persisted field or table, document and verify:

1. why the data must survive process restart;
2. which authority owns it and why no existing authority already contains it;
3. its stable semantic identity;
4. write/read lifecycle, concurrency, failure reporting, and recovery behavior;
5. retention, reset, export, and deletion semantics;
6. whether it contains secrets, personal data, or broker-authoritative state;
7. schema/version migration and deterministic tests; and
8. the typed boundary through which non-owning layers access it.

If any answer is unknown, stop and ask before changing a schema or save path.

The following are forbidden:

- generic settings, key-value, or JSON-blob tables used to avoid modeling
  ownership;
- storing the same authoritative value in both `.tbw` and SQLite;
- storing credentials or secret material in either profiles or databases;
- persisting derived UI read models or live broker snapshots as an alternate
  truth;
- direct SQL outside the persistence implementation;
- database reads or writes from rendering code;
- feature-local files, registry keys, ImGui INI data, or static/global state as
  unofficial persistence;
- adding a table first and deciding its owner or lifecycle afterward.

Database schema code and engine mechanics belong in `persistence`. Domain
meaning and invariants remain in their semantic owner, exposed to persistence
through typed records/interfaces. The application or adapters may orchestrate
when persistence happens through established APIs; the GUI never decides the
durability of domain state.

## Windows and workstation state

`tradebox::workstation::WorkstationState` and `tradebox::ui::Workspace` define
the current window/state system. Every new persistent window must use that
system.

In particular:

- use stable semantic type and instance IDs, never visible labels, symbols, or
  vector indexes as identity;
- define persistent semantic fields in the workstation state model;
- add compiled defaults, validation limits, deterministic profile encoding,
  and encode/decode round-trip tests;
- render persistent windows through `Workspace::BeginWindow` and
  `Workspace::EndWindow`;
- keep broker/core projections and ephemeral interaction state out of the
  workstation profile;
- keep `.tbw` as the sole authority for UI/application persistence; do not use
  ImGui INI state, SQLite UI settings, static locals, globals, or feature-local
  files as alternate persistence;
- distinguish closing a persistent window from deleting its document/state.

If a task depends on a primitive that is not implemented, report the gap and
ask the user to design or authorize the system extension first. A speculative
idea or old plan outside the repository is not implementation authority.

## Reuse rules

- Extend the owning abstraction instead of branching around it.
- Generalize shared behavior at its owner when the established design already
  provides a place for that generalization.
- Do not duplicate state to make access more convenient.
- Do not add feature-specific managers, caches, event loops, save paths,
  geometry handling, dirty tracking, ID schemes, error models, or validation
  when an owning system already exists.
- Do not use raw strings or booleans as a shadow protocol when typed domain
  types and commands exist.
- Do not weaken or bypass a system because integrating with it requires more
  files or tests.
- Do not preserve obsolete paths for compatibility unless the user explicitly
  requires a migration strategy.

## Engineering constraints

- The project standard is C++23. Use the standard library and the project's
  established idioms before introducing custom utilities or older compatibility
  patterns.
- Preserve the headless core's portability: no Windows, WinHTTP, SDL, ImGui,
  JSON, SQLite, or other adapter dependencies in `tradebox_core`.
- Do not add or upgrade a dependency without explicit user approval. Dependency
  versions are pinned deliberately in `CMakeLists.txt`.
- Build through `build.bat` from a Windows shell (`cmd //c build.bat` in
  git-bash, `.\build.bat` in PowerShell). It locates the installed Visual Studio
  C++ toolchain itself and loads the vcvars64 environment, so no VS developer
  prompt or manual PATH/INCLUDE/LIB setup is needed. Do not invoke raw `cmake`
  from a bare shell and do not reuse a stale `build/` cache after a toolchain
  change; the script reconfigures (`--fresh`) automatically when the detected
  compiler differs from the recorded one.
- Keep strict compiler warnings clean. Do not suppress a warning globally or
  weaken a build/test check to make a change pass.
- Add tests for behavior, regression fixes, serialization, and boundary rules
  at the layer that owns them. Do not test domain semantics only through GUI
  code.
- Do not edit generated, downloaded, vendored, or build-output files. Change
  their source or generation configuration instead.
- Preserve unrelated user changes in a dirty worktree. Never discard, rewrite,
  or reformat files outside the requested change merely for cleanup.
- Prefer typed values, explicit errors, deterministic behavior, RAII, and clear
  ownership/lifetimes. Do not introduce globals or hidden mutable state.

## Source of truth

The current source code, public types, CMake target graph, tests, and enforced
architecture checks are the repository's source of truth. Read them directly.
The root `README.md` is only a short orientation and build entry point, not a
detailed behavioral contract.

Do not add design documents, implementation plans, handoff notes, speculative
roadmaps, or duplicate Markdown descriptions unless the user explicitly asks
for one. Encode durable rules in types and executable tests whenever possible.
If the code and tests express conflicting ownership or behavior, stop and ask
which direction should become authoritative.

## Verification and completion

A change is complete only when it is integrated into the owning system. Verify
the smallest relevant tests first, then the broader affected suite. Preserve
architecture tests and add one when a boundary is important enough to prevent
future regressions.

In the final report, name the existing system used and list verification
performed. If blocked, report the architectural gap instead of presenting a
partial workaround as progress.
