# Raiju — Architecture

## Overview

```mermaid
flowchart TB
    subgraph LAN["Any device on the LAN"]
        Browser["Browser: Raiju UI"]
        Ext["Browser extension"]
        Companion["Desktop companion (Tauri/Rust)<br/>discovery · payload send · tray"]
    end

    subgraph PS5["PS5 · raiju.elf"]
        Gateway["Gateway (HTTP :2997)<br/>/ · /jsonrpc · /api/*"]
        Engine["Engine: aria2<br/>RPC on 127.0.0.1:6800"]
        Processor["Processor<br/>verify → extract → atomic install"]
        Supervisor["Supervisor<br/>privileges · session · keep-awake · notifications"]
    end

    Internet(("Internet")) -->|file data| Engine
    Browser & Ext & Companion -->|HTTP + WebSocket| Gateway
    Gateway -->|JSON-RPC proxy| Engine
    Gateway --> Processor
    Engine -->|writes| Staging[("/data/raiju/")]
    Processor -->|"rename()"| Homebrew[("/data/homebrew/")]
    Processor -->|"POST /api/v1/scan"| SMP["ShadowMountPlus :10101"]
    Supervisor -.-> Engine & Gateway & Processor
```

Data path for a download: **Internet → PS5 NIC → aria2 → NVMe → (extract) → rename into place**. Clients only send commands and receive progress; no file data passes through them.

## Components

### 1. Payload: `raiju.elf` (C/C++, ps5-payload-sdk)

Single ELF, sent through the existing ELF loader (port 9021).

**Supervisor**
- Runs with the privileges the loader grants, and owns no kernel state it did not create (ADR 0006). Measured on firmware 13600007 (#2, #46): the loader starts a payload with `euid 0` and no jail (`jaildir 0`), and that context alone creates the data root, writes and renames a staging file, takes the lock and reads `/data` space — so Raiju takes no credentials and borrows no jail. Escalation stays out until an operation is proven to need it, and then only in the narrowest step that works, recorded with its measurement and reverted on every exit path.
- Creates `/data/raiju/{downloads,staging,trash,session,www}` on first run.
- Starts the engine and gateway, and shows a PS5 notification: `Raiju ready — http://<ip>:2997 · code 4821`.
- Single-instance guard: if Raiju is already running, the new copy exits and re-shows the notification. An `flock` on `/data/raiju/session/raiju.lock` answers that, and the second launch is refused with `EWOULDBLOCK` (#2).
- Ends through `payload_exit()`. Returning from `main` hands the hijacked process back to its C++ teardown, which dies with an uncaught `system_error` a few milliseconds later (#2).

**Engine: aria2**
- Upstream aria2 1.37.0, with the connection cap patched — one hunk, `payload/third_party/patches/aria2-connections.patch`. Not the `motrixapp/aria2` fork: its headline feature is that one integer inside 107 changed files, and its SQLite session persistence is redundant because Raiju owns job state and the engine is an executor (ADR 0001).
- Build: static, `--without-gnutls --with-openssl --with-libz --without-libxml2 --without-sqlite3` for M0; add c-ares, then libxml2/sqlite later.
- Integration mode: **embedded `libaria2`** (`--enable-libaria2`) in a thread inside `raiju.elf`, with `enable-rpc=true` on loopback (ADR 0001). A standalone `aria2c` cannot work — a payload cannot spawn a program it built, because the SDK's binaries are `ET_DYN` with no `PT_INTERP` and `prospero-clang -static` is rejected for the PS5 target — so there is nothing left to choose between.
- aria2 runs a single event loop on FreeBSD `kqueue`. TLS on a Zen 2 core with AES-NI handles well over 1 Gbps, so one thread is not the bottleneck.

**Processor (post-download pipeline)**

Runs in its own worker thread. When aria2 reports a download complete (`onDownloadComplete`), the payload passes the processor a "download complete" event with the file (or folder) path and the size the server announced; the processor picks up that job. Each job is a small state machine, persisted to `/data/raiju/session/jobs.json`. On startup it resumes from the last completed stage.

```mermaid
stateDiagram-v2
    direction LR
    [*] --> Downloading
    Downloading --> Verifying
    Verifying --> Classifying
    Classifying --> Installing: image
    Classifying --> Extracting: archive
    Classifying --> Installing: other
    Classifying --> AwaitingTitleId: no title ID
    Extracting --> Installing
    Extracting --> AwaitingTitleId: no title ID
    AwaitingTitleId --> Installing: user enters ID
    Installing --> Conflict: name taken
    Conflict --> Installing: rename / replace
    Conflict --> Cancelled: cancel
    Extracting --> Cancelled: cancel
    Installing --> Rescan
    Rescan --> Notifying
    Notifying --> Ready
    Ready --> [*]
    Downloading --> Failed
    Verifying --> Failed
    Classifying --> Failed
    Extracting --> Failed
    Installing --> Failed
    Rescan --> Failed
    Notifying --> Failed
    Failed --> Downloading: retry
    note right of Failed: keeps files; retry returns to the failed stage (Downloading after a download or verify failure), or discard
```

The code lives in `payload/processor`: `job.h` (the job model), `job_store.h` (jobs.json), `pipeline.h` (one stage at a time), `processor.h` (the queue and its events) and `space.h`.

- **Stage semantics.** A job's `stage` is the next stage to run, and it only advances once that stage's work is done, so after a crash the stage that was running simply runs again. Every stage is safe to rerun: extraction wipes `staging/<job-id>/` first, and the install step recognises a move that already happened (see step 4).
- **jobs.json.** `{"version":1,"jobs":[…]}` with one flat object per job: stage, failed stage and error, download path, original name and the display name `param.json` gave, expected and uncompressed sizes, install root, keep-archive flag, conflict policy, kind, payload type and path, title ID and its source, committed install path, conflict choice, and extraction progress. It is rewritten after every stage: written to `jobs.json.tmp`, fsynced, renamed over `jobs.json`, then the directory is fsynced, so a crash leaves the old or the new file, never a mix. On load the file is untrusted: it is capped at 4 MiB and 512 jobs, parsed by a strict JSON reader (nesting capped, escapes validated, no NUL in strings), job IDs must match `[A-Za-z0-9_-]{1,64}` because they name the staging and trash folders, paths must be absolute without `.`/`..` components, every enum value must be known, IDs unique, and each job must carry the fields its stage needs. Unknown keys are skipped. A file that fails these checks is renamed to `jobs.json.bad` and the queue starts empty; files on disk are left alone. Before moving anything, the install step also checks that the payload lies inside the job's own download or staging folder.
- **Hooks.** Rescan (step 5) and the "ready" notification are function pointers supplied by the payload, so ShadowMountPlus and PS5 notifications plug in without the pipeline knowing about them; host tests use fakes. Either hook failing fails the job at that stage, and retry runs it again.
- **Retry and discard.** Retry sends a failed job back to the stage it failed in, except download and verify failures, which go back to Downloading because the bytes on disk are wrong. Discard deletes the job's staging and trash folders and, unless the job already installed, its download. Nothing under an install root is ever deleted by discard. `DELETE /api/jobs/:id` exposes it and refuses with 409 `wrong_stage` a job the processor is running or the engine is still downloading, so a cancel comes first and an extraction in flight is stopped.
- **Cancel.** Cancelling during extraction or at a conflict ends the job as Cancelled: its staging folder is removed and the download is kept until it is discarded.

1. **Verify.** Final size equals `Content-Length`. No checksum is expected; integrity comes from archive CRCs during extraction (step 3), the image's structural checks (step 2b), and the confirmed mount (step 6).
2. **Classify.** Look at the file's first bytes (magic), not just the extension:

   | Kind | Detection |
   |---|---|
   | 7z | `37 7A BC AF 27 1C` |
   | zip | `50 4B 03 04` |
   | rar | `52 61 72 21 1A 07` |
   | zstd / xz / gzip (tar.*) | `28 B5 2F FD` / `FD 37 7A 58 5A 00` / `1F 8B` |
   | `.exfat` image | `EXFAT   ` at offset 3 |
   | `.ffpkg` (UFS2) image | UFS2 superblock magic `0x19540119` at +1372, searched at 65536, 8192, 0 and 262144 (FreeBSD SBLOCKSEARCH), with `fs_sblockloc` (+1000) equal to the location |
   | `.ffpfs` / `.ffpfsc` | extension + PFS header check |

   An image with the wrong header, e.g. an HTML error page saved as `.ffpkg`, fails the job instead of being installed.

   **2b. Structural check (images).** exFAT: verify the boot-region checksum (sector 11 of the boot region), that the volume length fits in the file, and the 64 KiB cluster size ShadowMountPlus recommends, warning otherwise. UFS2: check the superblock's block/fragment sizes and that the image size covers the filesystem size.

   **2c. Identify the title.** Decide the install name `<TITLE_ID>`:
   1. Read `sce_sys/param.json` → top-level `titleId`, which must be four uppercase letters and five digits; a malformed document or value counts as missing. For archives, from the entry while extracting. For folder dumps, from disk. For images, read-only through a minimal exFAT/UFS2 reader that only walks root → `sce_sys` → `param.json`. The readers go through a `pread`-style I/O callback, treat every on-disk value as untrusted, cap `param.json` at 1 MiB and each directory at 4 MiB, and reject FAT chains that loop or end early. exFAT names match case-insensitively with ASCII folding only; UFS2 names match exactly, and the superblock is located like FreeBSD does it: the first of 65536, 8192, 0 and 262144 whose magic matches and whose `fs_sblockloc` equals that location, so backup copies are never mistaken for the primary.
   2. Otherwise, use a regex on the original file or archive name: `(PPSA|CUSA)\d{5}`.
   3. Otherwise, pause the job and ask the user in the UI.
   `param.json`'s display name (`localizedParameters`) is stored with the job for the UI. If (1) and (2) disagree, (1) wins and the UI shows a note.
3. **Extract** (archives only). libarchive reads the archive in order and writes into `/data/raiju/staging/<job-id>/`.
   - Every entry path is normalized and rejected if absolute or containing `..` or a backslash. Symlinks, hardlinks and special files inside archives are refused. Any rejected entry fails the job; nothing is silently skipped. libarchive's `SECURE_SYMLINKS` and `SECURE_NODOTDOT` flags are set as a second line of defence.
   - Large write buffers (1 MiB) and no per-file fsync during extraction; one `fsync` pass over the result happens before install.
   - Layout normalization: a folder dump must have `sce_sys/` at its root. If the archive wraps it in extra folders (`Game/Game/sce_sys`), Raiju strips them by installing the innermost folder; `__MACOSX/` and `.DS_Store` don't count as content. Otherwise, if the archive contains exactly one SMP image (`.ffpkg`, `.exfat`, `.ffpfs`, `.ffpfsc`) that passes the step 2 checks, that image becomes the install payload. Several images, or neither a dump nor an image, fail the job.
   - Progress is reported as bytes extracted out of the total, read from the archive index by the probe when the job is added (unknown for tar.*) and stored with the job.
   - Pause and cancel are honoured between entries. Resuming restarts the archive from the beginning, because libarchive can't resume mid-stream; the staging folder is wiped first. Extraction refuses a non-empty staging folder, so a resume can't mix two runs.
4. **Install.** `rename()` from staging (or from downloads, for images) to the destination. It's atomic because `/data/raiju` and `/data/homebrew` are on the same filesystem.
   - Destination defaults: images go to `/data/homebrew/<TITLE_ID>.<ext>` (extension in lowercase, e.g. `PPSA24003.exfat`), folder dumps to `/data/homebrew/<TITLE_ID>/`. The root is configurable, e.g. `/data/etaHEN/games`. The extension is the image's own when it has one (`Game.EXFAT` → `.exfat`), otherwise the one its magic bytes imply (`exfat`, `ffpkg`). Other files keep their original name in the chosen folder.
   - Installing takes two persisted steps. First the destination is decided and saved as the job's install path; only then does the second step `fsync` the payload (one pass over every file and folder, replacing per-file fsync during extraction) and rename it. A rerun after a crash that finds the payload gone and the destination present treats the move as done.
   - If the name already exists, the job waits in Conflict for the user to choose rename, replace or cancel; a job can also carry a preset policy (rename or replace) that answers without asking. "Rename" installs under the first free `<name>-<n>` from `-2` (`PPSA24003-2.exfat`, `PPSA24003-2/`, `homebrew-2.elf`). "Replace" first renames the old copy to `/data/raiju/trash/<job-id>/`, outside any scanned folder, then renames the new copy into place and deletes the trash folder; a crash at any point resumes the swap, and the install root always holds either the old or the new copy. The free-name check and the rename are not one atomic operation; that is safe because Raiju is the only writer in the install root.
5. **Confirm the install.** ShadowMountPlus scans `/data/homebrew` on its own interval (default 15 s), so installing is usually all the telling it needs: measured on firmware 13600007 (#7), a real 813 MB image renamed into place was picked up within one interval — mounted, registered and **unmounted again in the same second** — with the stock `config.ini`. Mounts are transient because `persistent_image_mounts=0` is the default: an image is mounted on demand when its game is launched, and released straight afterwards. Confirmation therefore reads ShadowMountPlus's own record — the registration it wrote, `mount_img.lnk`, or the API's game list — never a live mount. Its public API does exist on loopback (`api_enabled`, `api_bind_address`, `api_port=10101` in `/data/shadowmount/config.ini`), but this build's routes differ from the ones first assumed (`/api/v1/scan` answers `unknown API route`), so any call is written against the running build. A failure to recognise the image is surfaced in the UI with ShadowMountPlus's error, and all of this is skipped silently when ShadowMountPlus is not running.
6. **Clean up.** Runs at the start of Notifying, before the "ready" notification: delete the staging folder and, unless keep-archive is set, the archive (or what is left of a downloaded folder once the dump was moved out of it). Images and other files were moved into place, so there is nothing left to delete.

Space accounting. Before download: `archive_size + uncompressed_size + 1 GiB margin ≤ free(/data)`, where free space is `f_bavail × f_frsize` from `statvfs` (injectable for tests). Images and other files count only their own size plus the margin. Adding a job with a known size runs this check and refuses the job when it doesn't fit; when free space can't be read the job is allowed. For 7z/zip the uncompressed size comes from the file index at the end of the archive, read with an HTTP range request. For tar.* it's unknown up front, so Raiju warns and estimates 1× the archive size.

Libraries: libarchive (BSD), built with zstd, xz/lzma, zlib and bzip2. The M0 benchmark (#6) measured 7z LZMA2 at 21.54 MB/s against zip's 69.53 and tar.zst's 43.15 on the console, so 7z LZMA2 is the bottleneck and 7-Zip's C decoder (`LzmaDec`/`Lzma2DecMt`, public domain) decodes it, on multiple threads when the archive was created with multiple blocks — see [ADR 0002](docs/adr/0002-archive-decoders.md).

**Gateway (HTTP server)**
- CivetWeb (MIT) is the embeddable server. Avoid Mongoose: its open-source license is GPLv2-only, which is incompatible with GPLv3.
- Serves the UI, which is embedded in the ELF at build time. If `/data/raiju/www` exists it takes precedence, which lets you update the UI without rebuilding the ELF.
- Reverse-proxies `/jsonrpc` (HTTP and WebSocket) to aria2 on loopback and injects the aria2 secret as the first `params` element. Clients never see that secret.
- Auth: on first visit the UI asks for the 4-digit pairing code shown on the PS5. `POST /api/pair` verifies the code in constant time with an exponential backoff on wrong attempts, then issues a long-lived device token. The token travels in the HTTP-only `raiju_device` cookie (extension clients send it as an `Authorization` header). `GET /api/devices` lists them and `DELETE /api/devices/:id` revokes one, both behind the token gate. Tokens can be revoked in Settings.
- Only `GET /api/status` (companion discovery) and the static UI are reachable without a token; the status document never carries the secret, the pairing code or any token.

The gateway lives in `payload/gateway` and builds and tests on macOS and Linux with its own host Makefile (`make -C payload/gateway test`, `sanitize`), independent of the PS5 SDK. It is organised as:

- `include/raiju/*.h` public headers; `src/` implementations; `tests/` with a tiny harness and a real integration test.
- `platform.h` + `src/platform_host.c` + `src/platform_ps5.c`: every console-only call (notifications, privilege escalation, disk usage, interface IP, firmware version, engine launch, RNG, CA bundle) sits behind this one interface, so the host build substitutes fakes and the ELF uses the console one (#32). This is what `GET /api/status`'s `"platform"` reports, through the gateway's `platform_name`. The console implementation raises toasts through the system service with the message JSON-escaped and truncated, reads space through `statfs` (not `statvfs`), finds the LAN address with `getifaddrs` skipping loopback and link-local, reports the firmware the kernel gives (`13.60` for `0x13600007`), takes randomness from the libc CSPRNG, starts the engine in-process, and names `/data/raiju/ca.pem`, which the payload installs from the embedded bundle on first run. **Escalation stays a no-op:** #2 and #46 measured that the loader's context already writes the data root, so the hook is there to be measured on a console, not used (ADR 0006).
- The console build (`payload/Makefile`) compiles these same sources for the SDK's FreeBSD target, with the processor, CivetWeb, `third_party/ps5/ca_bundle.c`, the generated asset table and the embedded engine (`src/engine_ps5.cpp`), and links `libaria2` with OpenSSL, libarchive and its decoders. `-D__BSD_VISIBLE=1` is what the console's headers need to expose the BSD surface the shared sources use (`flock`, `u_int`, `INADDR_LOOPBACK`) — `_POSIX_C_SOURCE` hides it there, where Darwin exposes the same surface through `_DARWIN_C_SOURCE`.
- `supervisor.c`: creates `/data/raiju/{downloads,staging,trash,session,www}`, takes the single-instance lock, generates the pairing code, writes `session/aria2.conf` and `session/aria2.secret`, starts the engine and shows the ready notification. A second copy that cannot take the lock asks the running instance on loopback for its code (`GET /api/pairing/code`, authenticated with the engine secret both copies read from `session/aria2.secret`) and re-shows the notification with it, falling back to `----` when nothing answers (#32).
- `engine_options.c`: renders the aria2 options from the tuning table below (RPC on loopback, session persistence) and persists the secret atomically.
- `assets.c` + `tools/gen_assets.c`: a build-time tool walks an embed directory (`ui/dist` by default, overridable with `EMBED_DIR`) and emits a `{path, bytes}` table; an absent directory yields an empty table. `www` wins over the embedded table.
- `proxy.c`: secret injection and a minimal loopback HTTP client; `server.c` adapts CivetWeb, bridges the `/jsonrpc` WebSocket to aria2's, and hands the `aria2.onDownloadComplete`/`onDownloadError` notification frames to the gateway.
- `fetch.c`: the injectable outbound HTTP(S) client for `/api/art` and `/api/probe` (see the fetch seam below).
- `api.c`, `api_probe.c`, `art.c`: the `/api` worker thread, queue serialisation and handlers, the probe and its zip/7z index readers, and the OrbisPatches/ProsperoPatches artwork parser and cache.
- `pairing.c` (device store under `session/devices.json`, written atomically), `allowlist.c` (the Security roots), `auth.c`, `http.c` (routing table + request/response helpers), `gateway.c` (routes and handlers).
- `torrent.c`: reads the name a `.torrent`'s own info dictionary carries, from the base64 contents a client uploads, so a torrent job has a source name to detect its title ID from. The base64 is decoded strictly and the bencoded structure is walked with a depth cap, because a torrent is untrusted input; the name is then held to the same rule as any other file name.
- Routing is a table of `raiju_route` handlers with the gateway as their context. Issue #17 adds `/api/status`, `/api/storage`, `/api/folders`, `/api/jobs`, `/api/probe` and `/api/art` by appending routes with `raiju_gateway_add_route` and implementing handlers — no change to the core. `/api/power/keep-awake` is #18.
- CivetWeb is fetched by the Makefile from the pinned `v1.16` release tarball, verified by sha256, into `payload/third_party/civetweb` (gitignored). It is built without TLS.

**Raiju system API (`/api`)**

| Endpoint | Auth | Purpose |
|---|---|---|
| `GET /api/status` | public | Version, uptime, firmware, IP, engine health |
| `GET /api/storage` | token | Total/free bytes of `/data` plus the games and staging figures |
| `GET /api/folders` | token | Browse download destinations under the allowlisted roots |
| `GET /api/jobs`, `GET /api/jobs/:id` | token | Pipeline state per download |
| `POST /api/jobs` | token | Add a download — a `uri`, or the base64 contents of a `.torrent` — with destination, title ID override, keep archive and conflict policy |
| `POST /api/jobs/:id/{retry,cancel,resolve}` | token | Retry a failed stage, cancel, answer a name-conflict prompt |
| `DELETE /api/jobs/:id` | token | Remove a job from the queue, deleting its staging and trash folders and, unless it already installed, its download; 409 `wrong_stage` while the processor is working on it, so cancel first |
| `POST /api/probe` | token | HEAD + range read a URL for size, kind, uncompressed size and a fits verdict |
| `GET /api/art/:id` | token | Cover art for a title ID from OrbisPatches/ProsperoPatches |
| `POST /api/power/keep-awake`, `POST /api/power/notifications` | token | Toggle a payload power option; body `{"on":bool}` (#18) |
| `POST /api/pair`, `DELETE /api/devices/:id` | mixed | Pairing and device token management |
| `GET /api/pairing/code` | loopback + engine secret | The running instance's pairing code, for a second copy of the payload re-showing the "already running" notification (#32) |

Every endpoint but `GET /api/status` needs a device token (cookie or `Authorization: Bearer`). `GET /api/pairing/code` is refused with 403 unless the peer is loopback *and* the request carries the engine secret: the copy asking has no token yet, and that secret is the one credential both copies already share on disk.

**Shapes.**

- `GET /api/status` → `{"name":"raiju","version":"…","uptime":N,"firmware":"…","platform":"ps5"|"host","ip":"…","engine":{"up":bool[,"version":"…"]}}`. Engine health asks aria2 `aria2.getVersion` through the upstream client; a down engine is reported (`"up":false`) without failing the response. The secret, firewall pairing code and tokens never appear.
- `GET /api/storage` → `{"totalBytes","freeBytes","gamesBytes","stagingBytes"}`. Total and free come from the platform `disk_usage` hook on `<data_dir>`; `gamesBytes` sums the regular files under the install root (`/data/homebrew`) and `stagingBytes` under `<data_dir>/staging`, both walked without following symlinks and recomputed at most every 30 s.
- `GET /api/folders?path=<dir>` → `{"roots":[{"id","label","path"}],"path":"<resolved>","entries":[{"name","path","directory"}]}`. `roots` are the UI's preset pills (`homebrew`, `etaHEN/games`, `Downloads`); the path defaults to the install root and must resolve inside a Security root, so `..` and symlink escapes are refused (403) and a missing path is 404.
- `GET /api/jobs` → `{"jobs":[…]}` and `GET /api/jobs/:id` → one job; each job is `{"id","stage","uiStage","status","titleId","sourceName","name","kind","installName","bytesTotal","bytesStageTotal","bytesDone","error"}`. `stage` is the raw processor stage; `uiStage` and `status` use the UI vocabulary (mapping below). `displayName` is the game's own name from `param.json`'s `localizedParameters`, empty until it has been read, while `name` stays the file that was downloaded so a client can show both. `bytesTotal` is the download's expected size, `bytesStageTotal` the total of the stage running now — the expected size while downloading, the archive's uncompressed size while extracting, and 0 when that size is unknown, which is what a `tar.*` download reports until it has been read — and `bytesDone` is the progress of that same stage.
- `POST /api/jobs` body → `{"id","uri"|"torrent","destination"?,"titleId"?,"fileName"?,"kind"?,"size"?,"uncompressedSize"?,"keepArchive"?,"conflict"?}`. The `id` must be `[A-Za-z0-9_-]{1,64}` and is the aria2 GID (so notifications map back); `destination` must be inside an allowlisted root. Because the `id` is also the engine's GID, and aria2 accepts a GID only as 16 hex characters, a client that generates one has to use that shape. Exactly one of `uri` and `torrent` is required: `torrent` is the base64 contents of a `.torrent` file, whose own info name becomes `sourceName` unless the request passes `fileName`, and it goes to the engine as `aria2.addTorrent` instead of `aria2.addUri`. Everything after that — the space check, the pipeline and the install — is the same as for a URL, so a torrent can be recovered, verified and installed like any other download. A torrent's size is unknown until aria2 has read it, so the space check treats it like a URL whose size the client did not state. The job is added to the processor with the pipeline options and the space check runs, refusing with 409 `{"error":"no_space",…}` when it does not fit; then the URI goes to aria2 through `aria2.addUri` with `{"gid":id,"dir":"<data_dir>/downloads"}` and the secret injected. A 201 returns the created job.
- `POST /api/jobs/:id/resolve` body → `{"choice":"rename"|"replace"|"cancel"}`.
- `POST /api/probe` body → `{"uri"}` → `{"size","kind","category","uncompressedSize","uncompressedKnown","estimated","fits","supported"[,"unsupportedReason"],"requiredBytes","freeBytes"}`. An image kind the console cannot mount is reported with `"supported":false` and the reason, so no client promises an install that will not mount: a `.ffpkg` is a UFS2 image and firmware 13.60 refuses to mount one (#49), measured in #7. It HEADs the URL for the size (falling back to a `Range: bytes=0-0` when the server omits Content-Length), classifies the leading bytes with the processor's `classify`, and for a zip or 7z reads the archive index with range requests to sum the uncompressed sizes (`tar.*` stays unknown and is marked `estimated`); the verdict comes from the processor's `space`. A 7z header is usually stored encoded (`kEncodedHeader`): the probe reads the packed header stream (at most 4 MiB) and decodes it in memory (at most 16 MiB) with liblzma, then parses it as a plain header — see [ADR 0005](docs/adr/0005-sevenz-encoded-headers.md); anything larger, a codec it cannot map, or more than one packed stream reports the size as unknown rather than guessing. Only one probe runs at a time: a second `POST /api/probe` answered while another is in flight is refused with 503 `{"error":"probe_busy"}`, so a paired client cannot make the console scan the LAN in parallel.
- `GET /api/art/<TITLE_ID>` → `{"titleId","name","icon","background","logo","source"}` or 404 `{"error":"not_found"}`, both with `Content-Type: application/json` and `Cache-Control: public, max-age=86400`. CUSA titles resolve through OrbisPatches, PPSA through ProsperoPatches; the result is cached for 24h as JSON under `<data_dir>/art/<TITLE_ID>.json`.

**Job stage → UI vocabulary (#19 builds the UI on it).** `stage` is the raw processor stage; `uiStage` and `status` map as:

| Processor stage | `uiStage` | `status` |
|---|---|---|
| downloading | download | active |
| verifying, classifying, awaiting_title_id | verify | active (waiting for awaiting_title_id) |
| extracting | extract | active |
| installing, rescan, notifying, conflict | install | active (waiting for conflict) |
| ready | ready | done |
| failed | the bucket of `failed_stage` | failed |
| cancelled | ready | failed |

**Worker-thread ownership and the published job snapshot.** The processor is not thread safe and its queue is owned by one thread. The gateway starts a single worker thread that owns `raiju_processor`; every request that mutates the queue (jobs create/retry/resolve/cancel) and the worker itself serialise on the queue lock. The worker wakes on a signal or a 400 ms tick and each pass applies aria2 notifications, polls aria2 `tellStatus` for jobs still downloading (so a completion is seen even without a websocket client), and runs `raiju_processor_run`, which drives verify → classify → extract → install → rescan → notify. A cancel records the job ID in a small cancel set (its own mutex, read by the extraction progress hook) before it takes the queue lock, so a cancel can stop a running extraction without waiting on the queue lock. The processor's rescan and notify hooks are supplied by the gateway: rescan POSTs `<SMP>/api/v1/scan` and treats an unreachable ShadowMountPlus as a silent skip, and notify goes through the platform hook, so the host build succeeds without ShadowMountPlus.

**The read path never waits for a stage.** `GET /api/jobs` and `GET /api/jobs/:id` read a published snapshot (`raiju_api_job_view`) instead of the queue, so a long extraction cannot stall them. The worker copies the queue into the snapshot under `view_lock` while it holds the queue lock (lock order queue → view); read handlers take `view_lock` alone, so they never take the queue lock. The copy is refreshed when a stage or a progress figure moved and at most every 200 ms; every mutating request forces one immediately before its response is sent (so a job is readable the instant `POST /api/jobs` returns 201), the worker forces one whenever it applies a completed-download event and otherwise republishes after each pass, and extraction progress republishes from the progress hook. The snapshot is `RAIJU_JOBS_MAX` (512) fixed-size records allocated once at registration — roughly 512 × 1.7 KB, about 900 KB — and freed at unregistration; registration fails if the allocation fails. `raiju_api_job_json` (the mutating handlers' reply) and the read path share one `fill_view`/view-JSON pair, so the stage/status/install-name mapping has a single implementation.

**Outbound fetch seam.** `/api/art` and `/api/probe` share one outbound HTTP(S) client (`raiju/fetch.h`) that performs a single GET or HEAD with an optional Range header, a socket timeout and a byte cap, and never follows redirects. It is injectable (`raiju_gateway_config.fetch`): the built-in client speaks plain HTTP and HTTPS through OpenSSL when the build linked it (`-DRAIJU_FETCH_TLS`), and tests pass a fake or point at a local fixture server. The gateway never reaches the internet from a test. Its context is a `const raiju_platform *`: the built-in client builds a TLS context per connection, trusting the PEM file the `ca_bundle_path` hook names when it returns a path, or the system trust store otherwise, and always verifies the peer. A store that fails to load — absent, or zero-length, which OpenSSL itself refuses — fails the fetch before any handshake, while a store that loads but did not issue the chain fails during verification; both are pinned by `payload/gateway/tests/test_tls.c` against frozen certificates that need no network and never expire. On the PS5 the hook returns the bundle shipped at `/data/raiju/ca.pem`. When the host parses as an IPv4 literal (`https://127.0.0.1/...`, a NAS on the LAN, say) verification matches the IP subjectAltName rather than a DNS name.

### 2. Web UI (`ui/`)

- **Stack:** Vite + React + TypeScript + Tailwind CSS v4 + shadcn/ui. It's a static SPA, with no SSR and no Next.js, because the PS5 serves it as plain files.
- **Data layer (`ui/src/data/`).** Jobs, stages, storage, preset folders, the probe, pairing and devices come from the Raiju `/api` through the typed client in `ui/src/api/`, because only the gateway knows the pipeline. aria2, through the gateway's `/jsonrpc` proxy, supplies what the API doesn't carry: speed, connection counts, per-download options, and the detail sheet's files, peers and servers, matched to jobs by GID. `ui/src/api/map.ts` holds the API→UI mapping (stage, kind, install name, error sentences) and is unit-tested.
  - Polling: `/api/jobs` once per second while the window is visible, `/api/status` every two seconds until the console answers, storage every ten seconds, and `/api/folders` once per connection. `aria2.onDownloadStart/Pause/Complete/Error` events over the WebSocket trigger an immediate refetch.
  - `GET /api/status` is public, so the UI separates "console off" from "not paired yet": a refused connection shows the unreachable screen with background retries, a 401 shows the pairing screen.
  - `VITE_DEMO=true`, or a plain dev server with no gateway, uses the simulated source in `ui/src/data/mockSource.ts`, so the UI works without a console.
- **Working against a console in development:** start the dev server with `VITE_GATEWAY_URL=http://<ps5>:2997`; the Vite config proxies `/api` and `/jsonrpc` to the console, so the `raiju_device` cookie stays first-party.
- **Budget:** ≤ 300 KB gzipped total, since it's embedded in the ELF.
- Design system and screens: see [DESIGN.md](DESIGN.md).

### 3. Desktop companion (`companion/`, M2)

- **Tauri 2:** a Rust backend with the same `ui/` web app as its front end, so the UI stays identical on every platform. It builds for macOS, Windows and Linux with small native binaries (no bundled Chromium).
- **The Rust backend does what a browser can't:**
  - **Discovery:** scan the local subnet for the Raiju gateway (`GET /api/status` on port 2997). Later: mDNS, if the PS5 side supports it.
  - **Payload send:** push `raiju.elf` (bundled with the app) to the ELF loader on TCP 9021. Detect "PS5 reachable but Raiju not running" and offer to send it.
  - **Tray and notifications:** menu-bar/tray speed readout, and native notifications on job completion.
  - **Wake (M3):** the Remote Play wake packet.
- **The companion never touches file data**, so it doesn't affect transfer speed. Rust is there for a small, fast, cross-platform shell.

### 4. Browser extension (`extension/`, M2)

A Manifest V3 extension that intercepts downloads and sends them to `http://<ps5>:2997/jsonrpc` with the device token. Until M2, the existing "Aria2 Integration" extension works pointed at the gateway.

## Speed: how Raiju matches Motrix

Motrix's speed is aria2 with good defaults. Raiju ships the same tuning, editable in Settings:

| Option | Raiju default | Why |
|---|---|---|
| `max-connection-per-server` | 16 (the patch allows up to 1024) | Parallel TCP streams beat per-connection throttling |
| `split` | 16 | Pieces in flight per task |
| `min-split-size` | 10M | Don't split small files into tiny ranges. It also caps parallelism — pieces = size / this — so a file has to be large enough for the connection count asked of it: measured in #4, a 256 MB file at the 20 MB default split about twelve ways whatever `split` said |
| `max-concurrent-downloads` | 5 | |
| `disk-cache` | 64M | Batches writes; fewer NVMe syncs |
| `file-allocation` | `none` | `posix_fallocate` is not implemented on `/data` (#7), so `falloc` and `prealloc` cannot work; `trunc`, a sparse `ftruncate`, is the only other mode the console can implement |
| `no-file-allocation-limit` | 64M | |
| `enable-http-keep-alive` | true | |
| `async-dns` + c-ares | on (M1) | Faster connection setup |
| `optimize-concurrent-downloads` | off (test) | |
| `socket-recv-buffer-size` | tune in M0 | Large TCP windows for high-latency servers |

Physical ceiling is the PS5's 1 GbE port (~110–117 MB/s). Wi-Fi rarely gets close to it.

### End-to-end time: the format matters more than the code

Total time in v1 = download + extraction (if any). Approximate figures for a 100 GB dump on wired gigabit:

| Hosted as | Download | Extraction | Total |
|---|---|---|---|
| `.exfat` (`.ffpkg` does not mount on 13.60 yet, #49) | ~15 min | none | **~15 min** |
| `.ffpfsc` | less than 15 min (compressed) | none; decompressed at play time | **< 15 min** |
| `.tar.zst` | ~14 min | ~2 min (zstd decodes at 1+ GB/s per core) | ~16 min |
| `.7z` LZMA2 | ~14 min | ~12–30 min (roughly 50–150 MB/s) | ~30–45 min |

Game assets are mostly already compressed, so LZMA usually saves only 5–15% of download size while costing the most time.

### Streaming extraction (M3)

aria2 writes pieces out of order, so v1 extracts only after the download completes. The theoretical maximum is a custom engine that:
- downloads parallel byte ranges into a reorder buffer;
- feeds the decompressor in order as the data arrives;
- writes straight into staging.

Total time then becomes max(download, extraction) instead of their sum, and the archive never occupies disk space. This only works for sequential formats (`tar.zst`, `tar.xz`); 7z and zip need random access to their index. Resume needs checkpoints at independent zstd frames, i.e. the seekable zstd format. Build it only if M1 benchmarks show extraction dominates.

## Language choices

| Part | Language | Why |
|---|---|---|
| `raiju.elf` (engine, processor, gateway) | C/C++ | aria2 and libarchive are C/C++ and already heavily optimized. The ps5-payload-sdk is a clang/C toolchain. No language change makes the network or LZMA faster. |
| Streaming engine (M3) | C, or Rust if a spike proves it builds | Rust can target the PS5 in principle (x86_64 FreeBSD target linked against the SDK's libc), but there's no established toolchain. Spike before committing. |
| Web UI | TypeScript (React, shadcn) | Shared by browser, companion and extension |
| Desktop companion | Rust (Tauri) | Small, fast, cross-platform; native networking for discovery and payload send |

## Persistence

- `save-session=/data/raiju/session/aria2.session`, `save-session-interval=10` and `force-save=true` so paused and completed tasks are written too. The file is a record, not a restore path: measured in #4, a session created with the same `save-session` comes up with nothing to download, and `input-file` does not change that in the embedded mode.
- **Raiju restores its own jobs.** It re-adds them from `/data/raiju/session/jobs.json`, and each resumes from the piece record in its `<file>.aria2` control file rather than from the bytes on disk — measured, of about 50 MB written before a restart, roughly 34 MB was fetched again. Nothing else survives a payload restart, which is why the job store is the source of truth and the engine an executor (ADR 0001).
- The supervisor also forces a session save on shutdown signals and before rest mode, if a suspend notification is available.

## Power states

| State | Raiju behaviour |
|---|---|
| On, screen on or off | Full speed |
| Idle timer about to trigger rest mode | **Keep-awake:** while any task is active, the supervisor periodically resets the idle timer with `sceSystemServicePowerTick`, `sceShellCoreUtilResetAutoPowerDownTimer` and `sceShellCoreUtilSignalUserInput` — all three exported by `libSceSystemService`, and confirmed callable from a payload on 13600007 (#8) — and `sceSystemServiceReceiveEvent` reports `before_sleep` (0x10000001) and `on_resume` (0x10000000), so the supervisor can also react to a rest mode it cannot prevent. Once the queue is empty, the console sleeps normally. |
| Rest mode | Userland is suspended, so **no downloading**. On resume: if the payload survived, aria2 retries its connections and continues. If not, re-send the ELF and Raiju re-adds its jobs, which resume from their control files. |
| Off / reboot | Payload is gone. Re-send the ELF and Raiju re-adds its jobs. |
| Wake from network (research) | The Remote Play wake packet (UDP 9302 on PS5) plus a registration credential can wake the console. The payload must still be re-sent or auto-loaded afterwards. |

Downloading during rest mode itself isn't possible from a payload.

Measured in #8, and still open there: the three tick calls and the event call all resolve and return
success from a payload, and the ticks run on a timer and stop when released. Whether the hold really
survives the console's own idle timeout for hours, and what the payload, its sockets and the jailbreak
do across a rest mode and resume, need a console left idle for longer than the rest-mode timer — a
payload run can be started and left, and `payload/spikes/power` reports socket health on every resume
event. Poll the event call no faster than a second or so: every one goes through LNC, and the system
logs a `getAppStatus` line for each.

## Security

- aria2 RPC binds to loopback only; the gateway is the only network entry point.
- Pairing code plus device tokens; no anonymous access.
- File paths from clients are resolved and checked against allowlisted roots (`/data/raiju`, `/data/pkg`, `/data/homebrew`, `/data/etaHEN/games`). This blocks `../` and symlink escapes.
- Archive entries go through the same checks: no absolute paths, no `..`, no symlinks or hardlinks, so a malicious archive can't write outside staging.
- LAN only; no port forwarding or UPnP for the gateway port.
- A client-supplied download URL is fetched both by the probe (`/api/probe`, HEAD plus ranged GETs) and by aria2 itself, so a paired client can make the console request any host it can reach. That is inherent to a downloader and no wider than the engine's own reach; both paths reject anything that is not `http`/`https`, cap the response, time out and refuse a URL that carries a control character or a space.

## Repository layout

```
raiju/
├─ payload/          # raiju.elf: supervisor, gateway, processor, API
│  ├─ src/
│  ├─ processor/     # portable C11 pipeline stages; `make -C payload/processor test` runs host unit tests (needs libarchive and pkg-config), `make -C payload/processor sanitize` runs them under ASan and UBSan
│  ├─ gateway/       # portable C11 HTTP gateway + pairing; `make -C payload/gateway test` runs host unit and aria2 integration tests, `make -C payload/gateway sanitize` runs them under ASan and UBSan; embeds `ui/dist` (needs a local `aria2c` for the integration test)
│  ├─ third_party/   # build scripts plus the fetched, gitignored sources: aria2 (upstream, with the connection patch), libarchive, openssl, zlib, zstd, xz, c-ares, civetweb
│  ├─ spikes/        # M0 spike payloads, one directory per question: privileges (#2, #46), tls (#3), the filesystem and ShadowMountPlus (#7), spawning and embedding the engine (#4, #5), connection counting and session restore (#4)
│  └─ Makefile
├─ ui/               # Vite + React + shadcn SPA (shared by all clients)
├─ companion/        # Tauri 2 desktop app (Rust), wraps ui/ (M2)
├─ extension/        # MV3 browser extension (M2)
├─ docker/           # reproducible toolchain image for ps5-payload-sdk
├─ docs/adr/         # architecture decision records
├─ PRODUCT.md  ARCHITECTURE.md  DESIGN.md
```

Build: `make ui` (outputs `ui/dist`) → `make payload` (embeds `ui/dist` and outputs `raiju.elf`) → `make send PS5_HOST=…` (pushes to port 9021).

The console needs its own copies of the third-party libraries, so `payload/third_party/build-tls.sh` pins zlib, OpenSSL 3 and a CA bundle by sha256, cross-compiles them statically into `payload/third_party/ps5/` (gitignored), and runs in CI as part of the payload job. On macOS the SDK's wrappers reach LLVM through `llvm-config`, so `LLVM_CONFIG` has to point at `llvm@18`; CI installs `llvm-18`, which is already on `PATH`. `build-aria2.sh` follows the same shape for aria2, applying the connection patch after it unpacks upstream, and `build-libarchive.sh` does the same for bzip2, xz, zstd and libarchive, which libarchive itself links against.

## Licensing

Raiju is **GPL-3.0-or-later**, matching ps5-payload-dev projects such as ftpsrv and elfldr. Its dependencies are compatible with that:
- aria2 is GPLv2-or-later, so it can be combined into a GPLv3 work. Avoid any dependency that is GPLv2-*only*.
- OpenSSL 3 is Apache-2.0, which is compatible with GPLv3 (unlike with GPLv2).
- libarchive, zstd and xz are BSD/0BSD-style; CivetWeb is MIT.
- Motrix is MIT: its configuration choices and UI patterns can be referenced freely.
- ShadowMountPlus is GPLv3. Raiju calls its HTTP API today, and could also reuse its code if ever needed, e.g. its image readers.

## Risks and M0 spikes

1. **C++ runtime and threading in the payload SDK** for aria2. Settled (#4): the engine builds and runs embedded in the payload.
2. **OpenSSL build for the PS5 target**, plus a CA bundle shipped at `/data/raiju/ca.pem`.
3. **Embedded libaria2 vs standalone** (integration mode A vs B). Settled: embedded (ADR 0001) — a payload cannot spawn a program it built, so there was no mode B to have.
4. **`posix_fallocate` support on `/data`**, and sustained write speed.
5. **Keep-awake API**, and whether the payload survives rest mode.
6. **libarchive build and throughput** on the PS5 for 7z LZMA2, zip, tar.zst and rar5. Measured in #6: libarchive cross-compiled with all four decoders, and per-format rates of 21.54, 69.53, 43.15 and 16.69 MB/s on 1 GiB samples with one 4 GiB 7z pass. [ADR 0002](docs/adr/0002-archive-decoders.md) settles the decoder question; a pass of 20 GB or more on a real dump is still owed, because anything smaller sits in a 16 GB console's page cache.
7. **ShadowMountPlus behavior with renamed-in images:** confirm a `rename()` into a scan path plus `/api/v1/scan` mounts immediately. Also check how `scan_depth` treats images placed directly in `/data/homebrew` vs in a subfolder.
