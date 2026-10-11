# 0006. Run with the privileges the loader grants, and own no kernel state we did not create

- Status: accepted
- Date: 2026-10-11
- Issue: #46

## Context

The supervisor needs to create `/data/raiju/{downloads,staging,trash,session,www}`, write and rename files there, take an `flock`, and read free space. #2 measured what a payload is given to do that with on firmware 13600007: the loader starts it with `euid 0` and no jail at all (`jaildir 0`), and `mkdir /data/raiju` succeeds **before** any escalation runs.

Against that background the payload still took the `ps5-payload-dev/ftpsrv` recipe wholesale: a hardcoded `authid`, all sixteen capability bytes set to `0xff`, `uid`/`ruid`/`svuid`/`rgid`/`svgid` zeroed, and — the part that matters here — the **jail of another process**, read from `SceSysCore` with `kernel_get_proc_jaildir()` and installed on our own pid with `kernel_set_proc_jaildir()`. The spike restored nothing.

A payload does not run as an ordinary app: it runs inside a hijacked system process, so a fault in it is a system-level fault, and another process's jail is a kernel object whose lifetime we neither own nor control. In the session that produced these measurements the console was later found off and needed a storage repair. The cause is not attributed — an unclean power loss produces that screen too, and nobody was watching — but the borrowed jail and the blanket capability grant were the only things we did that could plausibly damage anything, so they are not worth their risk.

## Decision

- Raiju runs in the context the loader provides. It does not escalate as a matter of course.
- It takes no kernel state it did not create. Specifically, it never borrows another process's jail, and it never grants itself a blanket capability set. If a jail is ever needed, Raiju creates and owns its own.
- If an operation is *proven* to need more privilege on a real console, the change is made in the narrowest step that works, is recorded with the measurement that justified it, and is reverted on every exit path.
- The spike now measures in that order: the loader's context first, then `uid 0` alone, and only then the wider set, restoring the credentials it read and verifying the restore.

## Consequences

- The risk of a payload leaving a system process's kernel object in a bad state is removed, along with the blanket capability grant.
- The privilege question is now answered by measurement rather than by copying another project, and the answer is expected to be "nothing is needed", since #2 already showed the data root is writable as granted.
- If some later operation does turn out to need privilege, it must be demonstrated on the console, which costs a handover each time — the price of not granting capabilities speculatively.
- The supervisor's exit path is unchanged: `payload_exit()` on every path, because returning from `main` runs the hijacked process's C++ teardown and dies there (#2).
