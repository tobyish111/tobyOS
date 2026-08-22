# Linux-fidelity arc — survey, plan, and running log

**Started 2026-08-22.** The tsh/XCU walk closed; this arc is "what else on the
Linux side": a full audit of the Linux personality surface, then fixing the
gaps in priority order. The governing thesis stands: tobyOS runs BOTH Linux
and Windows software; Track C is co-equal; never break a personality boundary.

## The audit (2026-08-22, four parallel surveys over the whole tree)

Full details live in the session that produced this file; the load-bearing
findings, ranked by blast radius:

### Tier 1 — fake-success syscalls (silent lies)
1. **inotify**: `inotify_init` returns an ARRAY INDEX (first caller gets
   fd 0 == stdin); `inotify_add_watch` returns a counter and registers
   nothing; `inotify_emit()` has zero callers.
2. **No close-on-exec tracking anywhere** — every creator dropped
   O_CLOEXEC/SOCK_CLOEXEC; fcntl F_GETFD/F_SETFD were `return 0`; every fd
   leaked across exec. `close_range` absent. **FIXED — see log.**
3. **File locking**: fcntl F_SETLK/F_SETLKW/F_GETLK and flock(2) all
   `return 0` — no exclusion behind "you hold the lock". **FIXED — see log.**
4. **shutdown(2)** is `return 0`; no TCP half-close exists (`tcp_shutdown`
   absent from tcp.c). Every send-then-SHUT_WR-then-read-EOF client hangs.
5. **Blocking accept() returns EAGAIN after 3 s** (LX_SOCK_DEF_ACCEPT_MS);
   blocking connect() gives up at 5 s with ECONNREFUSED where ETIMEDOUT
   belongs.
6. **epoll**: 64-fd cap per instance (ENOMEM on the 65th add), EPOLLET and
   EPOLLONESHOT stored but never honoured, EPOLLRDHUP never produced, and
   TCP POLLOUT ignores cwnd/snd_wnd (says writable while send returns
   EAGAIN — busy-spin).

### Tier 2 — thread-group semantics
- **SIG_MAX = 32: no realtime signals.** glibc SIGCANCEL=32 / SIGSETXID=33
  get EINVAL → `pthread_cancel` and threaded `setuid` are broken. sigset is
  a uint32_t; masks convert with `<<1`/`>>1` in three places.
- Per-thread sighand SNAPSHOT (no CLONE_SIGHAND): a handler installed after
  pthread_create is invisible to siblings. Leader-indirection template:
  `proc_fds()` (syscall.c).
- A fatal signal (SIGSEGV/SIGKILL) exits only the THREAD (`proc_exit`, not
  `proc_exit_group`); `kill(pid, sig)` reaches only the leader slot.
- `getpid()` returns the TID for threads (documented open item); execve does
  not kill sibling threads; cwd and brk are per-thread.
- `set_robust_list` is a no-op (FUTEX_OWNER_DIED never set);
  PTHREAD_PROCESS_SHARED futexes cannot match (futex keys are (cr3, uaddr)).

### Tier 3 — networking fidelity
- **No loopback datapath** (`lo` is advertised to getifaddrs but
  127.0.0.1 routes to the gateway and dies).
- SO_REUSEADDR no-op while bind unconditionally rejects in-use ports;
  nearly all sockopts accept-and-discard; **getsockopt at non-SOL_SOCKET
  levels returns 0 WITHOUT writing optval** (caller reads stack garbage).
- /etc/resolv.conf frozen at 10.0.2.3 (SLIRP); the DHCP rewrite fails on
  read-only ramfs — DNS is dead on real hardware.
- No ICMP errors (no ECONNREFUSED for UDP, no PMTU), no raw sockets, no
  AF_INET6 sockets, no AF_PACKET. socketpair dropped SOCK_NONBLOCK/CLOEXEC
  (**fixed with the cloexec slice**). sendmsg silently truncates at 16
  iovecs. sendto drops its flags argument.
- lxposix has ZERO socket coverage; the owed test is "read a non-blocking
  socket BEFORE data arrives, assert EAGAIN".

### Tier 4 — /proc //sys //dev fidelity
- /proc/self/maps is three fabricated lines (no libraries/mmaps) — breaks
  crash handlers, sanitizers, JITs. cmdline is name+\n, not NUL-separated
  argv. status lacks Tgid/Vm*/Threads/Sig*/Cap*.
- No /proc/sys at all; no /proc/net at all; /proc/self/mounts ENOENT while
  /proc/mounts works (per-pid dispatch has no arm).
- Two namespace bugs: readlink("/proc/self") reports the HOST pid
  (procfs.c:1237); open("/proc/<pid>/ns/…") skips pid translation
  (syscall.c open path).
- /dev is not a directory; /dev/shm does not exist → glibc shm_open()
  (open("/dev/shm/…")) fails. Raw LX_stat(4)/LX_lstat(6) on synthesized
  /dev nodes still ENOENT (the dev_synth fix covered statx/newfstatat/
  access only). /dev/snd opens but cannot be listed (no getdents arm).
- sysfs is a 96-node static read-only table (256-byte files).

### Tier 5 — missing syscall families
xattr (all 12), POSIX timers (timer_create family; setitimer ignores
ITIMER_VIRTUAL/PROF), copy_file_range (coreutils 9 / Go use unconditionally),
sendfile/splice, restart_syscall, rt_sigqueueinfo, execveat, sendmmsg/
recvmmsg, SysV sem/msg (shm exists), mknod. rlimits fabricated and
prlimit64/getrlimit disagree.
**Trap found:** comments at syscall.c (census contract) claim io_uring and
bpf have explicit ENOSYS arms — they DON'T; first probe turns the gate red
while the comment says it can't. Also: the statx DRM arm writes a
`struct lx_stat` into a statx buffer (known, still live).

### Tier 6 — loader/auxv
No vDSO (every clock_gettime is a real syscall); AT_UID/EUID/GID/EGID
hardcoded 0 (glibc secure-mode reads these); AT_EXECFN/AT_PLATFORM/HWCAP2
absent; no /etc/ld.so.cache (every glibc-dynamic spawn needs explicit
LD_LIBRARY_PATH).

### Standing debts (from earlier arcs, unchanged)
Filesystem: no hard links anywhere, ext4+fat32 no rename, statfs fabricated,
ramfs `/` cannot create files (why the resolv.conf rewrite fails). Kernel:
vfork private stack (blocks `unshare -f` tooling), scheduler starves
woken-from-stop procs while the waker spins, TCP tiny-window lead open.
Hardware: EliteDesk runs owed (CW_SANDBOX flip, xHCI EP0 re-test, slice-7
non-root).

**Toolchain correction:** the conformance handoff's "this box has no Linux
toolchain" is FALSE — `.glibc-tc/x86-64--glibc--stable-2025.08-1` is a full
static-glibc sysroot and `programs/linux-gapfill/build.sh` uses it. New
acceptance tests are cheap; there is no longer an excuse for the owed ones.

## Order of attack
1. Un-red the gate (linux-timers wedge). **DONE**
2. Tier 1 honesty pass (cloexec ✅, locks ✅, shutdown/accept/connect,
   inotify, epoll).
3. Thread-group semantics slice (SIG_MAX 64 + shared sighand + group-fatal
   + getpid→tgid + process-directed delivery).
4. Networking-fidelity slice (loopback, SO_REUSEADDR, getsockopt, epoll cap/
   ET) + pay the owed EAGAIN socket gate.
5. /proc //dev fidelity (maps, cmdline, /dev directory + /dev/shm tmpfs,
   minimal /proc/net + /proc/sys, the two ns bugs, LX_stat dev arm).
6. Syscall long tail (xattr, POSIX timers, copy_file_range, …) + census
   comment fix.
7. Hardware validation batch when the EliteDesk is next available.

## Running log

### 2026-08-22 — the gate was RED: a fully-parked machine was a wedge (370442c)
`lxposix.sh` had been red since 2026-08-16, hanging at `linux-timers`
subtest 5 with ZERO heartbeats. QMP: all four CPUs HLT=1, static RIPs in
sched_yield/sched_idle, IF set — lost wakeup, not a spin. Root cause: every
wake source is a SWEEP driven from scheduler entry points, and the two
halted-CPU loops never re-ran them; slice 128 (sleepers park) made the
all-parked shape reachable for the first time. Fix: sched_halted_wake_sweeps()
in both halt loops + poll_unpark_current() for the case where the halted
proc IS the parked poller (poll_wake_all deliberately skips current). GREEN
10/10, 547 heartbeats, 0 faults.

### 2026-08-22 — close-on-exec exists now (verified 11/11)
`fd_cloexec` bitmap in struct proc (leader-indirected like proc_fds);
honoured by open/openat/openat2/dup3/pipe2/socket/accept4/socketpair/
eventfd2/memfd/timerfd/signalfd4/epoll_create1 + fcntl F_GETFD/F_SETFD/
F_DUPFD_CLOEXEC; acted on at BOTH exec commit points (ELF + PE);
close_range(436) with CLOSE_RANGE_CLOEXEC. socketpair's dropped
SOCK_NONBLOCK fixed in the same pass. New /bin/linux-cloexec (static glibc,
6 bits — bit3 asserts survival/closure from INSIDE an exec'd child) rides
lxposix. GREEN 11/11.

### 2026-08-22 — thread-group semantics: NPTL works (lxposix 14/14)
SIG_MAX 32→64 (sigset_t → u64 end-to-end; ucontext layout unchanged — the
old u32+pad occupied the same bytes; signals 1..63, Linux's 64 refused).
Shared sighand via `sig_actions_of()` leader indirection (the proc_fds
pattern). Fatal signals are GROUP-fatal (`proc_exit_group` at both signal
sites AND isr.c's user-fault fatal path, which also stops reporting -1 and
reports 128+sig). kill() is process-directed (retargets to a group member
with the signal unblocked); tkill/tgkill get their own thread-directed
entry point `sys_tgkill` with a REAL tgid check. Linux getpid() returns
the tgid. The futex WAIT path refuses to park with a deliverable signal
pending (lost-wake guard).

**Three chained root causes, each found by the previous fix:**
1. All-parked wedge: futex park vs signal_send's woke-only-if-BLOCKED
   (guard above).
2. Then a live hang: **glibc's NPTL handlers (sigcancel_handler,
   sighandler_setxid) begin with `if (si_pid != getpid() || si_code !=
   SI_TKILL) return;`** — an anti-spoof check. Our siginfo said SI_USER
   for tgkill signals, so the setxid handler ran and silently did
   NOTHING, forever. signal_state now records si_code per signal;
   sys_tgkill stamps SI_TKILL and the sender's TGID (glibc compares
   si_pid against ITS getpid). Diagnosed with -DFUTEX_TRACE (new opt-in:
   every futex op, capped — only sane on low-traffic boots) which showed
   the handler resume the barrier wait with no setuid and no ack-wake.
3. Then bit4: the ISR fault path killed only the faulting THREAD
   (proc_exit(-1)) — group-fatality had to be applied there separately.

`[idlewedge]` census added: the BSP halt loop prints every blocked proc's
signal/futex state once after 5 s of fruitless halting — a wedge is
silent by definition, and three theories fit the same silence until the
kernel names what everyone is waiting for.

Test: `linux-nptl` (real glibc NPTL, 63/63): create/join, **pthread_cancel
(first time ever possible)**, **threaded setuid observed from the OTHER
thread**, handler installed post-create fires in a sibling, a worker's
uncaught fatal signal kills the whole group (abort-flavoured so the gate's
fault census stays clean; the hardware flavour is the same one-line fix in
isr.c), getpid()==tgid alongside distinct gettid. Cross-personality gates
+ defboot re-run per the signals/sched law.

### 2026-08-22 — networking honesty batch (verified: lxposix 13/13 + lxsock GREEN)
shutdown(2) is real: `tcp_shutdown_tx` (FIN now, keep receiving; factored
from tcp_close's opening move) + `shut_tx/shut_rd/rx_eof` in struct sock.
AF_UNIX SHUT_WR marks the peer `rx_eof` WITHOUT severing the link (peer
still sends; severing is what full close does). Sends after SHUT_WR are
EPIPE with SIGPIPE first; reads after SHUT_RD are EOF; poll/sock_recv_ready
kept in lockstep. Blocking accept() waits INDEFINITELY (was: EAGAIN at 3 s;
SO_RCVTIMEO still bounds it); blocking connect() classifies ECONNREFUSED vs
ETIMEDOUT through the same lx_conn_progress machinery as the nb path.
getsockopt at non-SOL_SOCKET levels zero-fills instead of returning success
over the caller's stack garbage. socketpair honours SOCK_NONBLOCK+CLOEXEC.

**FOUND ON THE WAY: pit-tick deadlines run ~15x slow under TCG.** The PIT is
programmed at 1 kHz but the guest only receives ~66 IRQs/s under TCG, so
every `pit_hz()*ms` deadline was inflated (measured: a "5 s" connect
deadline classified at 75651 ms). conn_deadline is perf_now_ns()-based now —
the same TSC-backed clock timerfd trusts. **Anything else timed via
pit_ticks/pit_hz (tcp_accept slices, TIME_WAIT, socket recv timeouts) is
still wrong by the same factor — open item.**

Tests: `linux-sock` (peer-less: AF_UNIX half-close incl. drain-then-EOF,
EPIPE, SHUT_RD, the OWED pre-data EAGAIN test on unix+UDP, nb+blocking
connect timeout classification via SO_SNDTIMEO) rides lxposix;
`logs/lxsock.sh` + `linux-sockserver` (host peer over SLIRP hostfwd, b14
pattern) proves the peer-required half: blocking accept past a deliberate
4.5 s connect delay, pre-data EAGAIN on real TCP, PONG-then-FIN visible on
the HOST side with the host's post-FIN line still delivered, EPIPE after
shutdown. Two test-protocol lessons are recorded in the files: a pre-data
probe is only valid while the peer is PROVABLY quiet (make it wait for a
RDY token), and a host-side EOF check must distinguish instant-empty from
timeout-empty.

### 2026-08-22 — record locks + flock are real (src/flock.c)
Process-owned byte-range fcntl locks (F_GETLK/F_SETLK/F_SETLKW + OFD
variants) with range carve/split, and description-owned flock(2), keyed on
the (mnt, ino, ino_gen)/node identity fstat already uses. The two families
are independent, as on Linux (the "conservative" cross-check is a
self-deadlock trap). Release: any-close drops the process's record locks on
that file (the POSIX wart, kept deliberately); flock dies with the open
file description (vfs_refs pointer identity); exit sweeps both. New
/bin/linux-flock: every assertion cross-process with pipe handshakes, every
refusal asserts its errno. Gate pending.
