# TradeBox

Professional C++23 trading core with native SDL/OpenGL/ImGui workstation.

The current vertical slice is deliberately read-only:

- Alpaca paper/live API credentials in Windows Credential Manager.
- Account snapshot over the Trading REST API.
- Persistent watchlist and window layout.
- Cached daily IEX bars with asynchronous Alpaca backfill.
- One market-data WebSocket shared by every chart.
- Live daily candles driven by trades and Alpaca daily-bar corrections.
- Batched raw market-event recording in SQLite for future replay.

There is no order submission in this version. See [VISION.md](VISION.md) for the
product boundary and replay/journal direction. See
[CHART_RENDERING.md](CHART_RENDERING.md) for the adaptive-resolution live chart
concept and rendering model.

## Architecture

TradeBox is organized as independently testable targets:

- `tradebox_core`: transport-independent commands, broker events, safety state,
  generation fencing, and deterministic state transitions.
- `tradebox_broker_alpaca`: Alpaca REST/WebSocket adapter.
- `tradebox_persistence`: SQLite persistence adapter.
- `tradebox_platform`: Windows credential storage.
- `tradebox_gui`: SDL/ImGui interaction surface, producing
  `TradeBoxNative.exe`.

The core target has no dependency on Windows UI, WinHTTP, JSON, SDL, or ImGui.
An architecture test enforces this boundary. Future CLI and LLM adapters will
use the same typed core API and immutable snapshots as the GUI.

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

These pins were checked against the upstream stable releases on 2026-07-28.
Upgrades are deliberate: update the pin, migrate affected APIs, compile, and
run the framebuffer smoke test before accepting a new version.

Application data is stored in `%LOCALAPPDATA%\TradeBox`, not in this OneDrive
source directory. SDL owns the native window/event loop, Dear ImGui renders
through OpenGL, and WinHTTP provides HTTPS and WebSocket transport.
ImGui's embedded scalable vector font is rasterized on demand, with its font
scale following the SDL window's display scale.

From a Visual Studio x64 developer shell:

```powershell
cmake -S . -B build-current -DBUILD_TESTING=ON
cmake --build build-current --config Release
ctest --test-dir build-current -C Release --output-on-failure
```

## Framebuffer captures

The **Screenshot** button in the custom title bar reads the OpenGL back buffer
into an SDL surface and saves a BMP under
`%LOCALAPPDATA%\TradeBox\screenshots`. The custom title bar is included because
it is part of the same OpenGL framebuffer.

Automated visual smoke tests use the same path without capturing the Windows
desktop:

```powershell
TradeBoxNative.exe --capture-framebuffer C:\temp\tradebox-smoke.bmp
```

The application renders three frames, saves the framebuffer, records the
capture on the replay timeline, and exits.
