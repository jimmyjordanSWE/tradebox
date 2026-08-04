# TradeBox documentation

This directory is intentionally small. The **Current contracts** section lists
behavior that the code and tests are expected to preserve. The **Product
direction and future proposals** section contains unimplemented ideas; those
are not implementation instructions until a proposal is promoted into a
current contract.

## Current contracts

Read these when changing the application:

| Document | Purpose |
| --- | --- |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Authority boundaries, event flow, persistence, and verification. |
| [HEADLESS_APPLICATION_API.md](HEADLESS_APPLICATION_API.md) | Typed command and read-model contract shared by GUI, CLI, automation, and LLM adapters. |
| [ORDER_MODEL.md](ORDER_MODEL.md) | Broker-native orders versus future TradeBox strategy orders. |
| [ORDER_POSITION_STATE_MACHINE.md](ORDER_POSITION_STATE_MACHINE.md) | Reconciliation, order/position state, numeric safety, and regression rules. |
| [SECURITY.md](SECURITY.md) | Current threat model and enforced safety controls. |
| [WINDOW_MODEL.md](WINDOW_MODEL.md) | Current floating workspace, window identity, persistence, and UI primitives. |
| [CHART_RENDERING.md](CHART_RENDERING.md) | Current chart adapter contract and explicitly separated chart proposals. |

The GUI is a view and command adapter. It renders immutable application
snapshots, owns temporary widget state, and emits typed commands. It does not
own broker truth, aggregate market data, validate domain rules, persist
business state, or perform reconciliation.

## Product direction and future proposals

- [VISION.md](VISION.md) is the long-term product direction and V1 boundary.
- [ETF_WATCH_WINDOW.md](ETF_WATCH_WINDOW.md) is a future combined-chart
  proposal. It is not an implemented window.

Future ideas should be added to one of these proposal documents first. Promote
an idea into a current contract only after the core boundary, public types,
tests, and implementation have been agreed. Do not create another dated
handoff document for an implementation task.

## Documentation rules

- Prefer one canonical statement over repeated copies in plans, prompts, and
  handoffs.
- Describe ownership explicitly: core/application owns semantics; UI owns
  presentation and ephemeral drafts; adapters translate external protocols.
- Mark unimplemented behavior as **future** and keep it out of current
  acceptance criteria.
- When behavior changes, update the current contract and its tests in the same
  change. Remove obsolete wording instead of leaving contradictory history.
- Keep upstream-library links beside the code that uses them when a precise
  API detail matters; this directory records TradeBox's resulting contract,
  not a copy of ImGui, SDL3, or ImPlot documentation.
