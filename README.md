# TradeBox

TradeBox is a C++23 headless trading core with a native Windows
SDL3/DirectX 11/Dear ImGui workstation and Alpaca adapters.

The code, public types, CMake targets, and tests are authoritative. AI agents
must follow [AGENTS.md](AGENTS.md) before changing the repository.

Important boundaries:

- `tradebox_core` owns broker-independent trading semantics and must remain
  independent of Windows, networking, JSON, SQLite, SDL, and ImGui.
- Clients render immutable application snapshots and submit typed commands.
- `.tbw` profiles store workstation state, SQLite stores operational and market
  data, and Windows Credential Manager stores secrets.

## Building

One command from any Windows shell — no Visual Studio developer prompt and no
manual PATH setup. `build.bat` locates an installed Visual Studio C++ toolchain
(preferring the project's pinned VS 18 Insiders install), loads its x64 MSVC
environment, and drives CMake + Ninja:

```
build.bat              build the TradeBoxNative app (Release configuration)
build.bat Debug        build the app as Debug
build.bat Release test build the app, then build and run the test suite
```

Requirements:

- Visual Studio Installer with the **Desktop development with C++** workload
  (MSVC, Windows SDK, and CMake/Ninja tools). The project currently pins the
  VS 18 Insiders toolchain; any functional MSVC found by the script works.
- `build.bat Release test` additionally requires **Python 3** on PATH (used by
  `gtest_discover_tests`).
- The first configure downloads dependencies via FetchContent (SDL3, Dear
  ImGui, GoogleTest, ...), so it needs network access.

`build.bat` reconfigures from scratch (`cmake --fresh`) automatically whenever
the detected compiler differs from the one recorded in `build/CMakeCache.txt`,
so a stale cache can never silently poison a build. Running raw `cmake` from a
plain prompt is not supported: MSVC's `cl.exe` requires the vcvars64
environment (`INCLUDE`/`LIB`), which only the script sets up.
