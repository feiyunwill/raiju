# Raiju — agent guide

Raiju is a PS5 payload that downloads straight to the console's NVMe, verifies and extracts dumps, and installs them for ShadowMountPlus. Read before changing behaviour:

- [PRODUCT.md](PRODUCT.md): features and user-facing behaviour.
- [ARCHITECTURE.md](ARCHITECTURE.md): components, pipeline, build, security.
- [DESIGN.md](DESIGN.md): UI design system, before any `ui/` work.
- [docs/adr/](docs/adr/): settled decisions, before proposing an alternative to one.

## Work loop

1. Pick an open, unassigned issue whose `Blocked by` issues are all closed, earliest milestone first, then lowest number (`gh issue list --milestone M0`). Take `needs-ps5` issues only when the human says they are at the console; otherwise move on to the next unblocked issue, even in a later milestone. Assign it to yourself.
2. Branch from `next`: `<type>/<issue>-<slug>`, e.g. `feat/12-gateway-auth`.
3. Work until every acceptance-criteria checkbox in the issue is met and `bun run check` passes.
4. Open a PR into `next` titled as a conventional commit, body containing `Closes #<issue>`, and tick the criteria you met.
5. When the work changes behaviour or settles a question, update the doc that owns it in the same PR: behaviour in PRODUCT/ARCHITECTURE/DESIGN, decisions as a new ADR.

`main` receives code only by merging `next` (a stable release). Never push to `main` directly.

## Hardware

Issues labelled `needs-ps5` require the human at a jailbroken, powered-on console. Build and verify everything you can off-console first, then stop and hand over: list the exact commands to run (`make -C payload send PS5_HOST=<ip>`), what to observe, and what to report back. Record measured results in the issue.

## Code

- Names carry the meaning. The only comments allowed are doc blocks (JSDoc or equivalent) directly before a class or function. This applies to every file type, including config and workflows.
- C in `payload/` stays portable where it can (the processor builds and tests on macOS/Linux), with PS5-only code isolated behind a small platform layer.
- Licence is GPL-3.0-or-later; every new dependency must be compatible with it.

## Commits

Conventional Commits, enforced by hooks and CI; scopes are listed in `commitlint.config.mjs`. Commit messages are plain: subject, optional body, no trailers (no `Co-Authored-By`).
