# 0003. Host-testable gateway behind a platform seam

- Status: accepted
- Date: 2026-10-11
- Issue: #16

## Context

The gateway (HTTP server, aria2 proxy, pairing) is the only component that faces the LAN, so it carries most of the security surface: path allowlisting, secret injection, token auth. Testing it only on a console would be slow and would leave the security code unexercised in CI. The processor (#25–#29) already set the precedent: portable C with a tiny host harness, and PS5-only effects behind an injectable seam.

The gateway also needs a third-party HTTP server. CivetWeb (MIT) is the choice recorded in ARCHITECTURE.md; we need a reproducible way to obtain it that does not vendor a large tree into git.

## Decision

- The gateway is portable C11 with its own host Makefile (`make -C payload/gateway test`, `sanitize`) that builds and runs on macOS and Linux, mirroring the processor.
- Every PS5-only call (notifications, privilege escalation, `statfs`, interface IP, firmware version, engine process launch, randomness, the TLS CA bundle path) is a function pointer in `raiju_platform` (`raiju/platform.h`). The host build links `src/platform_host.c`; tests substitute a deterministic fake. The PS5 implementation is deferred to the payload build (#1). #17 added the `firmware` hook; gateway hardening added the `ca_bundle_path` hook.
- CivetWeb is fetched by the Makefile from the pinned `v1.16` release tarball, its sha256 verified, and extracted into `payload/third_party/civetweb`, which is gitignored. It is built without TLS (`NO_SSL`, `NO_CGI`, `USE_WEBSOCKET`).
- Host tests include a real integration test that starts a local `aria2c` with the generated options file, starts the gateway against it on an ephemeral port, and drives the pairing, HTTP proxy and WebSocket paths. When `aria2c` is absent the engine part is skipped with a printed reason, never silently.
- The embedded UI is generated at build time by a small C tool (`tools/gen_assets.c`) that walks an embed directory into a `{path, bytes}` table; no Node/Python dependency is added.

## Consequences

- The routing table (`raiju_route`) plus request/response helpers let issue #17 add the remaining `/api` endpoints without touching the core.
- Security-relevant code (allowlist, secret injection, token auth, pairing rate limiting) is covered by host unit tests and CI on both platforms.
- The gateway binary builds and runs on a desktop, which the tests and the CLI's `--no-engine` flag rely on. The server has no authentication bypass: every `/api` call but `GET /api/status` and the static UI needs a device token, in tests as on the console.
- Two implementations of `raiju_platform` must stay in sync: the host one here and the PS5 one added with #1.
