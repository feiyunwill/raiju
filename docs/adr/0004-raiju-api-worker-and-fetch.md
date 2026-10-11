# 0004. Raiju API: a single-owner worker thread and an injectable fetch seam

- Status: accepted
- Date: 2026-10-11
- Issue: #17

## Context

Issue #17 adds the `/api` surface (`status`, `storage`, `folders`, `jobs`, `probe`, `art`) on top of the gateway from #16. Three forces drove the design:

- The processor is explicitly **not thread safe** and its queue is owned by one thread, yet CivetWeb serves requests from a pool of threads. The `/api/jobs` handlers and the download-completion events both need to touch that queue.
- Downloads finish asynchronously. The gateway learns of completion from aria2's WebSocket notifications, but a payload must not depend on a browser being connected to receive them.
- `/api/art` and `/api/probe` must reach the internet (OrbisPatches/ProsperoPatches, and the download URL), yet every test must run offline, and the gateway must still build where OpenSSL is absent.

## Decision

- **One worker thread owns the processor.** `raiju_api` holds the `raiju_processor`, a single mutex, and a worker thread. Every request that touches the queue and the worker itself serialise on that mutex. This mirrors the processor's own contract: the queue has exactly one owner.
- **Completion is polled as well as pushed.** The worker wakes on a signal or a 400 ms tick and each pass applies any queued aria2 notifications (parsed from the WebSocket bridge in `server.c`), polls `aria2.tellStatus` for jobs still downloading, and runs `raiju_processor_run`. Polling makes a completed download progress even when no websocket client is subscribed.
- **Cancel is cooperative during a long stage.** A cancel records the job ID in a small cancel set behind its own mutex *before* it takes the queue mutex; the extraction progress hook returns `RAIJU_EXTRACT_CANCEL` for a listed ID. This lets a cancel stop a running extraction without blocking on the queue lock, while a cancel of an idle job is applied directly. The processor gained `raiju_processor_cancel` and `raiju_job_cancel` for the queue-level case (staging removed, download kept, install root untouched).
- **The outbound client is a seam.** `raiju/fetch.h` defines a single `GET`/`HEAD` with an optional Range header, a timeout and a byte cap, and never follows redirects. The gateway config carries the function pointer and its context; the built-in client is plain HTTP plus HTTPS through OpenSSL behind `-DRAIJU_FETCH_TLS`, chosen by the Makefile when pkg-config finds OpenSSL. Tests pass a fake or a local fixture server.
- **PS5-only calls stay behind `platform.h`.** Issue #17 added a `firmware` hook; the host reports `unknown`. The TLS path stays in portable C, compiled only when OpenSSL is available and linked in the CI gateway job on both platforms.

## Consequences

- The queue is never raced: every endpoint that mutates jobs takes the worker's mutex. The read endpoints no longer do. The worker publishes a fixed-size snapshot of the queue under its own `view_lock` (lock order queue → view), refreshed when a stage or a progress figure changed and at most every 200 ms, forced immediately on every state change, so `GET /api/jobs` and `GET /api/jobs/:id` never wait for a stage the worker is running. The snapshot costs about 900 KB (`RAIJU_JOBS_MAX` records, allocated once). The cancel path stays responsive because it signals through its own mutex first.
- A 400 ms polling tick adds a small, bounded load on aria2 while a download is active; it is silent otherwise (the loop returns immediately when no job is downloading).
- The gateway now links the portable processor (and therefore libarchive); its Makefile detects libarchive the same way the processor's does, and the CI gateway job installs libarchive-dev/`brew install libarchive` alongside OpenSSL.
- The TLS path is exercised hermetically: a test generates a self-signed certificate in process, runs a local TLS server and fetches through it, proving the CA-bundle hook is honoured and verification is enforced. It should still be smoke-tested once against a real host when the PS5 build lands.
