# TradeBox

TradeBox is a C++23 trading core with a native SDL/DirectX 11/ImGui workstation.
V1 connects to Alpaca for market data and broker operations while keeping
broker reality, durable recovery, and user-visible safety state inside the
headless core.

Current V1 capabilities include:

- Paper and live account credentials stored in Windows Credential Manager.
- Reconciled account, order, and position state from REST and WebSocket events.
- Typed submit, replace, cancel, cancel-all, and emergency-close commands.
- Durable command intent/recovery and idempotent client order identifiers.
- Persistent, stable-ID market data across supported candlestick timeframes.
- Shared supervised WebSockets with reconnect, gap recovery, and surfaced
  connection/staleness status.
- Core-owned mark-to-market valuation from the latest usable price.
- Batched raw market/account event recording in SQLite for audit and replay.

The GUI is a client of the core: it renders immutable snapshots and decides how
to present typed status and errors. It does not infer broker truth or silently
hide transport, validation, persistence, or reconciliation failures.

## Architecture

TradeBox is organized as independently testable targets:

- `tradebox_core`: transport-independent commands, broker events, safety state,
  generation fencing, and deterministic state transitions.
- `tradebox_broker_alpaca`: Alpaca REST/WebSocket adapter.
- `tradebox_persistence`: SQLite persistence adapter.
- `tradebox_platform`: Windows credential storage.
- `tradebox_gui`: SDL/ImGui interaction surface, producing
  `TradeBoxNative.exe`.

The core has no dependency on Windows UI, WinHTTP, JSON, SDL, or ImGui. An
architecture test enforces this boundary. Future CLI and LLM adapters can use
the same typed commands and immutable snapshots as the GUI.

See the durable project documentation:

- [Architecture and V1 guarantees](docs/ARCHITECTURE.md)
- [Headless application API](docs/HEADLESS_APPLICATION_API.md)
- [Security model](docs/SECURITY.md)
- [Broker and strategy order model](docs/ORDER_MODEL.md)
- [Order and position consistency](docs/ORDER_POSITION_STATE_MACHINE.md)
- [Product vision and replay model](docs/VISION.md)

## Toolchain and build

The supported toolchain is 64-bit Visual Studio 2022 Build Tools 17.14 / MSVC
19.44 or newer with a C++23 standard library containing `std::expected`.
The currently verified generator is Ninja through CMake 3.24 or newer.

Open this folder as a CMake project in Visual Studio 2022. CMake downloads the
exact dependency versions below at configure time, then builds the
`tradebox_gui` target for `x64`.

| Dependency | Pinned stable version |
| --- | --- |
| SDL | 3.4.12 |
| Dear ImGui | 1.92.9 |
| nlohmann/json | 3.12.0 |
| SQLite amalgamation | 3.53.4 |
| GoogleTest | 1.17.0 |

Dependency upgrades are deliberate: update the pin, migrate affected APIs,
compile, and run the framebuffer smoke test before accepting a new version.

Application data is stored in `%LOCALAPPDATA%\TradeBox`, outside this OneDrive
source directory. SDL owns the native window/event loop, Dear ImGui renders
through DirectX 11, and WinHTTP provides HTTPS and WebSocket transport.

From a Visual Studio x64 developer shell:

```powershell
cmake -S . -B build-current -DBUILD_TESTING=ON
cmake --build build-current --config Release
ctest --test-dir build-current -C Release --output-on-failure
```

The opt-in Alpaca Paper contract test is documented in
[tests/contract/README.md](tests/contract/README.md).

## Framebuffer captures

The **Screenshot** button in the custom title bar reads the DirectX 11 back
buffer through a staging texture into an SDL surface and saves a BMP under
`%LOCALAPPDATA%\TradeBox\screenshots`.

Automated visual smoke tests use the same path without capturing the Windows
desktop:

```powershell
TradeBoxNative.exe --capture-framebuffer C:\temp\tradebox-smoke.bmp
```

The application renders three frames, saves the framebuffer, records the
capture on the replay timeline, and exits.
