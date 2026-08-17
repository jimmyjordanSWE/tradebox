# TradeBox

TradeBox is an experimental native Windows trading workstation and an
unofficial front end for the Alpaca Trading API.

I started building it because I wanted a controlled, straightforward way to
place and manage stock orders through Alpaca. The longer-term goal is a useful
order-management workstation where entering, monitoring, replacing, and
canceling orders is quick without hiding what is being sent to the broker.

> [!WARNING]
> TradeBox is under heavy development and can submit real orders when connected
> to a live Alpaca account. It may contain bugs, incomplete behavior, or rough
> safety edges. Start with Alpaca Paper Trading, inspect every order, and use it
> entirely at your own risk. This is personal software, not financial advice,
> and there is no promise that it is suitable for production trading.

## Current state

The stock-trading and order-management foundation works: TradeBox connects to
Alpaca, consumes live and historical market data, displays account state,
positions and orders, and supports the stock order lifecycle. The underlying
trading core, application boundaries, persistence, reconciliation, and test
coverage are considerably further along than the interface.

The user interface is very much a work in progress. Charts are actively being
built and are only partway finished, and several workflows still need design
and polish. There is no installer or stable release yet, so expect to build the
application from source and expect things to change.

Only Alpaca stock trading is currently implemented. Options and crypto trading
are not implemented.

## How it was built

The product direction, interaction goals, safety boundaries, and architecture
are human-directed. A large part of the implementation has been developed
through AI-assisted "vibe coding," backed by strict compiler warnings,
automated tests, and explicit architecture checks.

TradeBox uses C++23 with a native SDL3, DirectX 11, and Dear ImGui workstation.
The broker-independent trading core is kept separate from Alpaca, Windows,
SQLite, and the GUI. Clients render immutable application snapshots and submit
typed commands. Workstation state is stored in `.tbw` profiles, operational and
market data is stored in SQLite, and API credentials are stored in Windows
Credential Manager.

TradeBox is an independent project and is not affiliated with, maintained by,
or endorsed by Alpaca.

## Building

Run one command from a normal Windows shell. No Visual Studio developer prompt
or manual compiler-path setup is needed:

```text
build.bat              build the TradeBoxNative app in Release mode
build.bat Debug        build the app in Debug mode
build.bat Release test build the app and run the non-credentialed test suite
```

Requirements:

- Windows with Visual Studio's **Desktop development with C++** workload
  installed, including MSVC, the Windows SDK, CMake, and Ninja.
- Python 3 on `PATH` when running the tests.
- Network access during the first build so CMake can download the pinned
  dependencies.

`build.bat` locates the Visual Studio toolchain, loads its x64 environment, and
reconfigures automatically when the compiler changes. Running raw CMake from a
plain shell is not the supported build path.

The code, public types, CMake targets, and tests are the implementation source
of truth. Contributors using AI coding agents should read [AGENTS.md](AGENTS.md)
before making changes.
