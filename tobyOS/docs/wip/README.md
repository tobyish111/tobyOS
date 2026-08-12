# ptrace — written, does not work, PARKED (2026-08-11)

`ptrace.c.wip` and `linux-ptrace.wip/` are a complete-looking ptrace subset and
its acceptance test. **They are parked out of the build on purpose: the tracer
and tracee do not hand off correctly and the test hangs.** Nothing here is
wired into the Makefile or the LXNS table, so the tree builds and gates green
without it.

Parked rather than deleted because the design is sound and the failure is
specific — but committing a kernel that wedges on a traced process would be
worse than not having the feature.

## What works

Everything except the handshake. It compiles clean, the request dispatch is
complete (TRACEME / ATTACH / SEIZE / DETACH / PEEK / POKE / GETREGS / SETREGS /
SETOPTIONS / CONT / SYSCALL / KILL), PEEK/POKE reuse the editor-root + HHDM
borrow that `process_vm_readv` uses and were written against the same model,
and unimplemented requests refuse with EINVAL rather than pretending.

## What is broken, precisely

The tracer blocks and the tracee never resumes it. Three attempts:

1. **First:** `wait4` looked only for `ptrace_stopped`, but strace's opening
   handshake is `PTRACE_TRACEME` then `raise(SIGSTOP)` — a JOB-CONTROL stop,
   which never set that flag. Tracer waits for a ptrace-stop, tracee sits in a
   job-control stop, neither moves. Fixed by marking `ptrace_stopped` at
   signal.c's stop site.
2. **Second:** that fix also woke the tracer with `sched_enqueue()` from the
   signal path, and the guest wedged **completely** — `heartbeats=0`, a hard
   hang, not a slow one. This is exactly what handoff §7 warns about
   (*"signal_send touches the run queue"*, and the `(cs & 3) == 3` gate on that
   path is a safety condition). Removed.
3. **Third:** with no run-queue work, `wait4`'s traced-child arm polls for the
   flag instead. The guest stays LIVE (556 heartbeats, 0 faults) but the test
   still never gets past its first stop, and the LXNS suite never reaches its
   verdict.

So the remaining bug is in the third arrangement: the tracee parks with
`state = PROC_STOPPED; sched_yield()`, and the polling tracer either never
observes `ptrace_stopped`, or observes it and its `PTRACE_SYSCALL` resume does
not actually get the tracee running again.

## Where to look first

- **Does the tracee reach the stop at all?** No instrument printed from inside
  `ptrace_stop`. A `kprintf` there costs one boot and splits the problem in
  half — tracee never stops, versus tracer never notices.
- **Is `sched_yield()` on a `PROC_STOPPED` proc actually a park?** signal.c's
  job-control stop relies on exactly that, so it should be — but the job-control
  path is entered from signal delivery, whereas a syscall-stop is entered from
  the middle of `linux_syscall` with the BKL held. **That difference is the
  most likely culprit and was never checked.** Blocking paths in this tree drop
  the BKL first (`bkl_held()` / `bkl_exit()`); `ptrace_stop` does not.
- The polling loop added to `wait4` holds no lock while spinning, which is
  right, but it re-reads `c->ptrace_stopped` without any barrier.

## The test is worth keeping

`linux-ptrace.wip/main.c` is a miniature strace rather than a list of `ptrace()`
calls, deliberately: "each request returned 0" would pass on a kernel where
every stop is fabricated and every register read is zeroes. The child performs a
syscall sequence it chooses and the tracer has to report it back by number, by
argument and by return value; bits 4 and 5 read and write memory that exists
only in the other address space. Reuse it as-is.

## Why it is worth finishing

Most of the session that produced this was spent inferring what a program
wanted from outside it. The syscall ring records argument POINTERS, so a
missing file reads as `openat a1=29407312 = -2` and names nothing — which cost
a dedicated kernel instrument (`-DPATHFAIL_TRACE`) and several twenty-minute
boots to answer. `strace` would have answered it in one.
