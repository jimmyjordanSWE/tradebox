# Alpaca Paper contract test

This test is credentialed, opt-in, and hard-coded to
`AccountEnvironment::Paper`. It cannot select the Live Trading host. It
creates two one-share Paper limit orders and cancels them.

The default symbol is `SPY`, with extended-hours-eligible resting buy
limits of `$1.00` and `$1.01`. This allows the order to become replaceable
in Alpaca's supported equity sessions while keeping it deliberately
non-marketable. If Alpaca leaves the order queued in `accepted`, the test
cancels it and reports a safe skip. Different safe, far-from-market limit
prices can be supplied explicitly.

Configure a separate build so the credentialed test is registered:

```powershell
cmake -S . -B build-paper-contract `
  -DBUILD_TESTING=ON `
  -DTRADEBOX_ENABLE_ALPACA_PAPER_CONTRACT_TESTS=ON
cmake --build build-paper-contract --config Release --target `
  tradebox_alpaca_paper_contract
```

The test first accepts Paper credentials from `APCA_API_KEY_ID` and
`APCA_API_SECRET_KEY`. If both are absent, it loads the existing saved
Paper credentials from TradeBox's Windows Credential Manager entry. It
never loads the Live credential entry.

Set the explicit destructive-test acknowledgement only in the process
that runs the test:

```powershell
$env:TRADEBOX_ALPACA_PAPER_CONTRACT = `
  "I_UNDERSTAND_THIS_CREATES_PAPER_ORDERS"
ctest --test-dir build-paper-contract -C Release `
  -R Contract.AlpacaPaperOrderLifecycle --output-on-failure
```

Optional settings:

```text
TRADEBOX_ALPACA_CONTRACT_SYMBOL
TRADEBOX_ALPACA_CONTRACT_LIMIT_PRICE
TRADEBOX_ALPACA_CONTRACT_REPLACEMENT_PRICE
```

If environment credentials are preferred, set both credential variables
in the same process before running CTest. If `APCA_API_BASE_URL` is
present, it must be exactly
`https://paper-api.alpaca.markets`. Credentials are read from environment
variables or Windows Credential Manager and are never written to the test
database or output.

The contract verifies:

- initial account, orders, and positions reconciliation;
- account `trade_updates` websocket acknowledgement;
- disconnect and reconnect with a new connection generation;
- place, REST snapshot observation, websocket observation, replace,
  cancel, and authoritative terminal state;
- trading rate-limit response headers;
- an injected loss after real broker dispatch but before durable command
  completion;
- restart recovery by deterministic `client_order_id`, exactly one broker
  order, and cancellation of the recovered order.

The ordinary build and `ctest` suite never register or run this
credentialed test.

If Alpaca keeps the order queued in `accepted`, CTest reports the contract
as skipped and the harness cancels the order. To audit and cancel any
nonterminal orders left by an interrupted contract run:

```powershell
$env:TRADEBOX_ALPACA_PAPER_CONTRACT = `
  "I_UNDERSTAND_THIS_CREATES_PAPER_ORDERS"
.\build-paper-contract\tradebox_alpaca_paper_contract.exe --cleanup-only
```

The cleanup audit treats `filled`, `canceled`, `expired`, `rejected`,
`replaced`, `done_for_day`, and `stopped` as terminal and fails if any
open `tb-contract-*` order remains.
