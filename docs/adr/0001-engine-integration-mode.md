# 0001. Run aria2 embedded and drive it in-process

- Status: accepted
- Date: 2026-10-11
- Issue: #5

## Context

The supervisor needs one thing from a download engine: take jobs from Raiju's own store, run them on the console at whatever speed the console and the source allow, and be told when they finish. #4 put two routes to that on a real console, and the console chose between them.

**Spawning a standalone `aria2c` cannot work.** A payload cannot start a program it built: `posix_spawn` of our own `aria2c` returns `Exec format error`, because the SDK's binaries are `ET_DYN` with no `PT_INTERP` — they are loaded by the loader that started us, not by a kernel that could start them again — and `prospero-clang -static` is rejected for the PS5 target, so there is no static build to fall back on. This is a property of the platform's loading model, not a matter of effort.

**RPC and AriaNg were nearly written off, wrongly.** libaria2's header reads "The behavior is very similar to RPC server, except that this option does not enable RPC functionality", which looks like a statement about the library and is in fact a statement about the `keepRunning` flag: the header's own opening says the library aims to provide the same functionality as the RPC methods. Measured on the console with `enable-rpc=true`, the embedded session served `aria2.getVersion`, `addUri`, `tellActive`, `pause`, `unpause` and `remove` to the laptop over the LAN, and paused a download for real — `completedLength` frozen at 8388608 across two readings five seconds apart, then climbing again after `unpause`. The gateway's design depends on this: it proxies `/jsonrpc` and injects the engine's secret, so an engine without RPC would have meant rebuilding that path around a different interface.

**What the engine costs and returns.** Over TLS, sixteen connections, 10.1 GB in 901 s: 11.2 MB/s average, 33 MB/s peak, 108.8 s user and 71.1 s system CPU. The path to the source is the limit, not the engine: the same engine measured 10.6 MB/s at sixteen connections and 10.5 MB/s at sixty-four, so connection count stops mattering long before upstream's cap of sixteen does. The cap is worth lifting anyway (`payload/third_party/patches/aria2-connections.patch`, one hunk on upstream 1.37.0), but only alongside `min-split-size`: pieces = size / that value, so a file has to be large enough for the connection count asked of it, and sixty-four connections on a 256 MB file quietly behaved like twelve.

## Decision

- The engine is **libaria2 embedded in `raiju.elf`** and driven in-process through the library API: `sessionNew`, `run(RUN_ONCE)`, `addUri`, `pause`, `unpause`, `remove`, and `shutdown` before `sessionFinal`.
- Raiju does not spawn a separate `aria2c`. It cannot, and with the embedded engine working there is nothing it would buy.
- **The engine's RPC stays enabled, on loopback, with a secret.** It binds to `127.0.0.1`, carries `rpc-secret`, and reaches the network only through the gateway's `/jsonrpc` proxy, which authenticates the client and injects the secret. `rpc-listen-all` does not appear in Raiju's options, and neither does an RPC without a secret. The spike that settled the availability question is also the argument for the rule: it set `rpc-listen-all` with no secret and left a port open on the console for four minutes that accepted `forceShutdown` from a laptop without a credential.
- Stay on upstream aria2 with that one-line patch rather than a fork. The `motrixapp/aria2` fork's headline feature is the connection cap, one integer inside 107 changed files, and its SQLite session store is redundant because Raiju owns job state and the engine is an executor.
- aria2 is GPL-2.0-or-later, compatible with Raiju's GPL-3.0-or-later.

## Consequences

- The engine lives and dies with the payload. There is no separate process to supervise, and no way for one to outlive a crash.
- **Shutting the engine down explicitly is now part of every exit path**, before `payload_exit()`, because #2 established that returning from `main` in a payload runs the hijacked process's teardown and dies there. That also means the engine's own cleanup never runs by itself: `shutdown` and `sessionFinal` are ours to call, and the last one is what writes the session file.
- **Restoring a download is the caller's job.** The session file is written but is not an input: a fresh session created with the same `save-session` has nothing to download, so Raiju re-adds jobs from its own store. Resume granularity is then the piece record in `<file>.aria2`, not the bytes on disk — measured, of about 50 MB written before a restart, roughly 34 MB was fetched again.
- Clients keep speaking to the gateway, the browser extension included, and AriaNg reaches the engine the same way — through `/jsonrpc`, where the token and the secret live. The engine's own port is not a surface clients are pointed at.
- The payload now contains a large C++ library. Its allocation and its shutdown ordering are payload concerns from here on.
- Speed work now has a measured ceiling: 33 MB/s peak on a gigabit link, with the path to the source as the limiter. Tuning connections further is not where the time will come from.
