# TradeBox

TradeBox is an experimental native Windows trading workstation and an
unofficial front end for the Alpaca Trading API.

I originally built it for myself because I wanted a controlled way to practice
trading with an Alpaca Paper Trading account. I did not want order entry to be
a mysterious automated black box: I wanted to see my account, positions,
orders, and market data in one place, deliberately construct an order, and
understand exactly what would be sent to Alpaca.

The longer-term idea is a practical order-management workstation that makes it
easy to enter, monitor, replace, and cancel orders while keeping the final
decision with the person using it. It can connect to either Alpaca Paper or
Live Trading, but Paper Trading is the intended place to learn the application
and try unfinished features.

> [!WARNING]
> TradeBox is under heavy development and can submit real orders when connected
> to a live Alpaca account. It may contain bugs, incomplete behavior, or rough
> safety edges. Start with Alpaca Paper Trading, inspect every order, and use it
> entirely at your own risk. This is personal software, not financial advice,
> and there is no promise that it is suitable for production trading.

## Download and run

TradeBox currently supports 64-bit Windows. Open the
[latest GitHub release](https://github.com/jimmyjordanSWE/tradebox/releases/latest)
and download this file—not GitHub's automatically generated source archives:

```text
TradeBox-v0.1.0-alpha-windows-x64.zip
```

Then:

1. Extract the entire ZIP to a normal folder.
2. Keep the `assets` directory beside `TradeBoxNative.exe`.
3. Run `TradeBoxNative.exe`.
4. Add your Alpaca Paper Trading API credentials through the account menu and
   connect to the Paper environment first.

The application is not code-signed and has no installer yet, so Windows may
show a SmartScreen warning. API credentials are stored in Windows Credential
Manager rather than in the workstation profile or SQLite databases.

The SHA-256 checksum file beside the ZIP can be used to confirm that the
download was not altered.

## What currently works

The stock-trading and order-management foundation is functional. Current work
includes:

- Alpaca Paper and Live account connections;
- account, position, and order views;
- stock order entry, cancellation, replacement, and bracket-order support;
- live account activity and market-data streams;
- historical and live stock bars;
- persistent watch lists and workstation layouts;
- operational recovery and broker reconciliation; and
- local operational and market-data databases.

The underlying trading core, application boundaries, persistence,
reconciliation, and automated tests are considerably further along than the
interface.

## What is unfinished

The user interface is very much a work in progress. Charts are actively being
built and are only partway finished, and several workflows still need design,
feedback, and polish. Releases should be treated as development snapshots, not
stable production builds.

Only Alpaca stock trading is currently implemented. Options and crypto trading
are not implemented. TradeBox is not a trading strategy, signal service, or
autonomous trading bot, and it does not make trading decisions for you.

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

## Building from source

Run one command from a normal Windows shell. No Visual Studio developer prompt
or manual compiler-path setup is needed:

```text
build.bat                 build TradeBoxNative in Release mode
build.bat Debug           build TradeBoxNative in Debug mode
build.bat Release test    build and run the non-credentialed test suite
build.bat Release package build and verify the release ZIP
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

## License

No public reuse license is currently granted for TradeBox itself. Third-party
components retain their respective licenses; release bundles include their
notices and license texts.
