# Security model

## Threat model

V1 treats the signed-in Windows user, the TradeBox process, and its local
application/source folders as trusted. Hostile local administrators, malware,
debuggers, memory readers, and a compromised executable are outside the current
threat model.

Network peers and all broker/provider payloads are untrusted. A valid TLS
connection does not make response content safe.

## Controls

- API credentials are stored in Windows Credential Manager rather than source,
  workspace, or SQLite files.
- Secret containers are move-only and wipe owned memory on destruction.
- HTTPS and secure WebSockets validate certificates; certificate revocation
  checks are enabled.
- HTTP redirects are disabled so credentials are not forwarded to a different
  endpoint through redirect handling.
- REST bodies are capped at 64 MiB.
- Market-data and account WebSocket messages are capped at 8 MiB.
- Remote data is parsed into typed domain values with size, numeric, enum,
  identity, generation, and state-transition validation.
- Transport, authentication, subscription, validation, persistence, and
  reconciliation failures are surfaced to clients as typed status/errors.
- Paper/live identity and command safety gates are enforced in the core, not
  delegated to presentation code.

## Deferred release hardening

Local SQLite ACL tightening or database encryption is not required by the
current trusted-local-machine model. Executable/code signing and release
distribution hardening are also deferred until TradeBox has an actual release
channel. Revisit both assumptions before distributing the application or
expanding the local threat model.
