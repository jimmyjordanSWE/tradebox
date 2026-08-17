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
[current alpha release](https://github.com/jimmyjordanSWE/tradebox/releases/tag/v0.1.1-alpha)
and download this file—not GitHub's automatically generated source archives:

```text
TradeBox-v0.1.1-alpha-windows-x64.zip
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

## What to expect

The underlying trading core is considerably further along than the interface.
It owns broker-independent order validation, price and quantity rules, order
state, account and market-data projections, recovery, and reconciliation. The
application layer exposes that state to the UI as read-only snapshots and
accepts typed trading commands. Alpaca-specific HTTP and streaming details,
SQLite storage, workstation profiles, and the GUI are kept outside the core.

The current Windows interface is usable as a paper-trading workstation, but it
is still rough. Today you can:

- connect to an Alpaca Paper or Live account and view account, position, and
  order state;
- enter, cancel, and replace stock orders, including bracket orders;
- receive live account activity and market data and load historical stock
  bars;
- create and rename watch lists, add, remove, and reorder tickers, and choose
  and sort quote columns;
- click a populated watch-list row to make that ticker the active symbol in
  the **Trade Hotkey** window;
- keep separate Trade Hotkey drafts for each ticker: stop-loss percentage,
  profit-target percentage, and GTC/day choice are restored when you switch
  tickers or reopen the workstation profile; and
- save window layout, open documents, tables, watch lists, chart state, and
  application risk limits in a `.tbw` workstation profile.

The Trade Hotkey workflow currently creates a **bracket order**: an entry order
with a take-profit exit and a stop-loss exit. The core and Alpaca adapter also
understand standalone OCO orders, but there is not yet a finished general OCO
order-entry window in the UI. In other words, selecting a watch-list ticker
changes the symbol used by the bracket-order Trade Hotkey window; it does not
silently place an order.

Operational order history and recovery data and local market history are kept
in separate SQLite databases. API credentials are never written to a `.tbw`
profile or those databases.

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
