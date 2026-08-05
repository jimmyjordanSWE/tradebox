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

Build from a Visual Studio 2022 x64 developer shell:

```powershell
cmake -S . -B build-current -DBUILD_TESTING=ON
cmake --build build-current --config Release
ctest --test-dir build-current -C Release --output-on-failure
```
