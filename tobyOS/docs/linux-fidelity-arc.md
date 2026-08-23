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

### 2026-08-22 — /proc + /dev fidelity batch (lxposix 17/17)
readlink(/proc/self) reports the READER's pid (was the host pid — the
last slice-10 rule violation); open(/proc/PID/ns/…) translates its
numeric component (was raw — wrong nsfd inside a pid namespace);
/proc/self/mounts exists (same generator as /proc/mounts — libmount,
findmnt, systemd and Go read the per-pid path and got ENOENT);
/proc/PID/cmdline is the REAL argv, NUL-separated, recorded at
spawn+execve in the PCB (was name+'\n'); raw stat(2)/lstat(2) see the
synthesised /dev nodes (the dev_synth fix's one missing arm); /dev
itself is openable + listable (synth-table getdents, /dev/snd included
— it opened but couldn't list); **/dev/shm is a real tmpfs mounted at
boot, so glibc's shm_open — literally open("/dev/shm/…") — works: POSIX
shared memory exists for Linux binaries for the first time** (memfd had
been carrying chrome instead). Longest-prefix mount resolution means the
mount works without /dev being a real directory. Test: `linux-procdev`
(63/63, incl. a full shm_open/ftruncate/MAP_SHARED/shm_unlink
round-trip). Still open here: real /proc/self/maps (needs the two-VMA-
system recon), /proc/sys, /proc/net.

### 2026-08-22 — syscall-tail batch (lxposix 16/16)
Real auxv credentials: AT_UID/EUID/GID/EGID from the process's actual ids
(user-namespace-translated, slice-11's boundary rule) instead of
hardcoded 0, and AT_SECURE computed from euid!=ruid — packed AFTER the
setuid-on-exec update, so suid images get glibc's secure mode (LD_* env
dropped), which is the protection suid exists for. sendmmsg/recvmmsg
loop over the proven single-message arms (only the first recv may
block). io_uring_setup/enter/register + bpf answer ENOSYS
AUTHORITATIVELY — the census-contract comments claimed those arms
existed and they did not, so the first probing workload would have
turned the gate red while the comment said it cannot. restart_syscall
answers EINTR. rt_sigqueueinfo delivers with real sender identity (sival
payload not carried — one pending bit per signal — documented).
statx-on-a-DRM-path emits real STATX layout (was a struct lx_stat
written into a statx buffer — the handoff's known latent defect,
closed). Test: `linux-misc` (63/63), incl. drop-to-1000-and-reexec so
the auxv assertion is non-trivial.

### 2026-08-22 — inotify is real; epoll stops lying (lxposix 15/15)
inotify: a REAL fd (FILE_KIND_INOTIFY, refcounted across dup/fork — the
test's forked child releasing the parent's instance was found live),
watches registered for real, and the VFS's four path-addressed mutators
(create incl. open(O_CREAT), unlink, rename, mkdir) emit events with
child names; rename FROM/TO pairs share a cookie. The matcher is
non-recursive (exact or direct-child), as Linux watchers assume.
fd-addressed vfs_write cannot emit IN_MODIFY — struct vfs_file carries no
path (the same missing identity behind /proc fd readlink → "/"); one open
item, not two. **FOUND: LX_inotify_add_watch/rm_watch were OFF BY ONE for
their entire life** (x86-64: init=253/add=254/rm=255) — invisible while
the arms were argument-ignoring fakes; the first honest implementation
surfaced it in one boot (glibc's add landed in the rm arm; its rm fell
into the ENOSYS census).

epoll: cap 64→512 (the 65th ADD answered ENOMEM); EPOLLONESHOT really
disarms until EPOLL_CTL_MOD; EPOLLRDHUP is produced (TCP HUP + AF_UNIX
peer-SHUT_WR); TCP POLLOUT now consults the send window (same gate as
tcp_send_nb — epoll said writable while send said EAGAIN, a busy-spin).
**EPOLLET is deliberately served as LEVEL**: a scan-based edge tracker
cannot see a drain-and-refill between two waits — precisely every real ET
app's loop — so honest edge-tracking would LOSE wakeups where level only
adds spurious ones. The test asserts the no-lost-wakeup contract.

Test: `linux-watch` (63/63): real-fd probe, IN_CREATE/DELETE with names,
rename cookie pair, poll wake from a forked child's create, 80-fd epoll,
ET no-loss, ONESHOT disarm/rearm.

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
refusal asserts its errno. (Gate went green in the 12/12 run that followed.)

### 2026-08-22 — the pit-skew purge + ICMP + SO_REUSEADDR (lxposix 18/18, netbatch GREEN)
Every remaining pit_ticks×pit_hz deadline in the network stack moved to
perf_now_ns: tcp_tick_one retransmit/RTT, TIME_WAIT, the service tick,
poll_until, connect/accept/send/recv/close deadlines, CUBIC's epoch. (The
PIT delivers ~66 of 1000 programmed IRQs under TCG, so every one of those
was ~15x slow — the "5 s" that measured 75 s.) SO_REUSEADDR is stored,
honoured by tcp_listen_reuse against TIME_WAIT ghosts, and readable back.
UDP-to-nobody now answers: icmp_send_port_unreach (rate-limited) + the
type-3 decode path into sock_udp_icmp_error, so a blocking recvfrom after a
refused send reports ECONNREFUSED like Linux instead of hanging. sendmmsg/
recvmmsg pack per-message results in the 64-byte-stride mmsghdr layout.

### 2026-08-22 — loopback exists, and it caught a TCP ordering bug (19fcddb)
127.0.0.0/8 (and self-addressed) IP delivers INLINE at the top of ip_send —
no device, no SLIRP round-trip. That immediacy exposed a latent ordering
bug: tcp_send_data_segment emitted the segment BEFORE accounting snd_nxt,
so with inline delivery the peer's ACK arrived while snd_nxt still pointed
at the OLD edge and SYN_RECEIVED rejected valid handshake ACKs (30-75 s
stalls). Now: account-before-emit, with tcp_emit_at(seq) for retransmits.
Loopback UDP checksums are elided (h->checksum=0); the ICMP emitter uses
src-as-orig-dst for 127/8 so refusal matching works on loops. lxsock stays
green — the restructure held under the host-peer gate.

### 2026-08-22 — procfs tells the truth (19e9b21, lxposix 19/19)
/proc/<pid>/maps is generated from the REAL vma tables (g_vma_tables, 8192
entries — mmap_vma_snapshot + brk range + stack, insertion-sorted, with a
truncation marker so a capped listing never silently reads as complete).
/proc/sys/{kernel,fs,vm} and /proc/net/{dev,tcp,udp,unix,route} exist with
live values (tcp from tcp_conn_snapshot in /proc/net/tcp hex format);
status gains Tgid/VmSize/VmRSS/Threads and the Sig* masks glibc's
pthread_kill error paths read. /proc/self readlink answers the VIRTUAL pid
(pid_vnr) — a PID-namespaced process must not see its host pid. cmdline is
recorded at spawn/exec (proc_record_cmdline, NUL-separated) instead of
synthesized.

### 2026-08-22 — POSIX timers + the kernel copy family (94477dd, lxposix 20/20)
timer_create/settime/gettime/getoverrun/delete (src/ptimer.c) fire from the
same ~10 ms alarm sweep + perf_now_ns clock the alarm machinery trusts;
catch-up is reported as OVERRUN, not a signal burst. sendfile/
copy_file_range/splice share one kernel copy loop (lx_copy_fds); offset
forms position through the CANONICAL lseek path — v1 poked fi->vfs.pos
directly and the shared-OFD position load silently overwrote it (read 0
bytes at EOF while claiming to advance the caller's offset). Test lesson in
linux-io: a wait that must span signal deliveries cannot be one usleep —
the first delivery returns it early; sleep in slices against the clock.

### 2026-08-22 — Phase E: xattrs + struct file learns its path (2262f4d, lxposix 21/21)
`struct file.open_path` (stamped at sys_open's mint, cloned on dup/fork,
freed at close) turned out to be ONE missing identity behind THREE
separately-recorded findings, and all closed together: /proc/<pid>/fd
readlink (answered "/" forever), the f* xattr forms (cp -a's actual
calls), and glibc fexecve — execveat is an authoritative ENOSYS BY
DESIGN, which routes glibc onto its execve("/proc/self/fd/N") fallback,
and that path resolves now (procfs stats fd links as symlinks; exec's
vfs_follow_link chases the answer). Bonus: fd-addressed IN_MODIFY emits.
xattrs: src/xattr.c, path-keyed store, user.* only; unlink forgets,
rename re-keys (directory-prefix aware); honesty contract in the file
header (tmpfs = real Linux semantics; tobyfs survives rename/unlink
within a boot, not across — chosen over failing every cp -a). mknod
creates S_IFREG for real, EPERM for the rest. Test trap recorded: the
harness spawns with a BARE argv[0]; self-reopening tests need /bin paths.

### 2026-08-22 — Phase F: de_thread + pshared futexes + robust mutexes (926ff61, 22/22)
proc_reap_group_threads() factored from proc_exit; sys_execve reaps the
group at its point of no return (image read OK — where the setuid
transition already sits). PROCESS_SHARED futexes key by (0, physaddr)
via vmm_translate_root (new explicit-root, BKL-free walk that also
reports PTE_SHARED_SW); FUTEX_PRIVATE_FLAG ops skip the walk; the
futex_fast WAKE arm keys identically or cross-process wakes die in the
fast path. set_robust_list stored NOTHING before; futex_robust_exit
walks the glibc list at every death site (exit, both group-reap loops,
exec), stamps FUTEX_OWNER_DIED, wakes one waiter under both keys.

TWO OLDER FORK LIES fell to the test: (1) sys_mmap's EAGER anon commit
dropped VMM_SHARED — anon-MAP_SHARED pages CoW'd apart at fork (demand
path was fixed in slice 22; small mappings never take it) — the child's
mutex lock landed in a private copy; (2) the clone process-fork arm
dropped CHILD_SETTID/CLEARTID, and glibc fork() IS
clone(CHILD_SETTID|CHILD_CLEARTID, &THREAD_SELF->tid) — every forked
child computed with its PARENT's cached tid. Staged like clone_ns_flags,
consumed at EMBRYO, stamped by fork_child_entry IN THE CHILD'S CONTEXT
(a parent-side write lands in the CoW-shared frame). Diagnostic method
that cracked it: a 4-line shared-write probe in the test separated
"memory not shared" from "futex keys not matching" in one boot.

### 2026-08-22 — Phase G: writable root + hard links (fb140ef, 23/23)
ramfs create/unlink/mkdir via an address-stable spillover table (open
handles hold raw node pointers — growth must never move slots); 256
created names, dead slots reused when their open refs drain;
unlink-while-open keeps bytes readable. tobyfs hard links: the on-disk
nlink existed unread since the format was born; vfs_link + ->link,
EXDEV across mounts, EPERM where no inode indirection exists. TWO MORE
LATENT BUGS fell: tobyfs handles flushed their OPEN-TIME inode copy on
every write (reverting nlink/mode/uid/gid changed while the fd was
open — handle_refresh_foreign re-reads them), and openat(dirfd, FILE)
was cwd-relative (sys_open split into resolve wrapper +
sys_open_resolved). Plus: orphan dirents (name → freed inode) are now
removable instead of poisoning their name with EEXIST forever. Test
discipline: /data persists across runs — tests sweep their own names.

### 2026-08-22 — Phase G: ld.so.cache (15e90f4, 24/24)
tools/mkldsocache.c (HOST tool; a python version silently never ran —
no python on the gate's PATH) writes a real glibc-ld.so.cache1.1 at
initrd build over every staged lib dir (294 libraries). Format traps:
string offsets are file-relative, the flag byte says little-endian (3),
and ld.so BINARY-SEARCHES under _dl_cache_libcmp in ldconfig's
DESCENDING order — an unsorted cache resolves only lucky names. Proof:
a real glibc-2.41 dynamic PIE (dlopen included) exits 127/127 with NO
LD_* in its environment.

### 2026-08-22 — Phase H: SysV sem + msg (f77d48f, 25/25)
src/sysvipc.c: semop with an atomic-or-nothing trial pass, cooperative
blocking, semtimedop on the perf clock, REAL SEM_UNDO applied at exit
(the Apache/PostgreSQL crashed-worker case, asserted live: a child dies
holding the semaphore and the parent's blocked P() proceeds). Message
queues with Linux's type selection. Ids carry a sequence number so
post-RMID operations answer EIDRM instead of landing on a recycled
slot (the slice-89 discipline).

### 2026-08-22 — Phase H: statfs truth + foreign-FS rename (8e114cb, 26/26)
vfs_statfs op per filesystem (tobyfs bitmaps, tmpfs budget, ramfs
image, ext group descriptors, fat32 FAT scan); the old arm fabricated
one 4 GiB tmpfs for EVERY mount since it was written. Asserted live:
64 KiB written to /data drops bfree by exactly 16×4 KiB blocks.
ext2/ext4/fat32 gain dirent-level rename (ext4 journalled; directories
same-parent only — a cross-dir move leaves ".." stale, and refusing
beats corrupting a foreign volume; FAT re-walks the source after a
clobber-unlink rewrites clusters). OWED: a manual FS_BOOT_SELFTESTS
boot with /ext + /vfat mounted to see the foreign-FS arms live.

## Arc close-out (2026-08-23)

**Final validation over HEAD 8e114cb, all green:**
- lxposix: 26/26 subtests, skipped=0, enosys_gaps=0, 0 faults
- cross-personality XPIPE/X2/3W: GREEN (Track C untouched by eight
  commits of kernel surgery)
- defboot: 3/3 alive, 0 faults
- lxsock host-peer: 15/15 GREEN

**The audit's six tiers, disposition:**
- Tier 1 (fake-success lies): CLOSED — cloexec, locks, shutdown/accept/
  connect, inotify, epoll.
- Tier 2 (thread groups): CLOSED — SIG_MAX 64, shared sighand,
  group-fatal, tgkill, exec de_thread, robust futexes, PROCESS_SHARED
  futexes. (exec-from-non-leader pid takeover remains a documented
  divergence; nothing exercised does it.)
- Tier 3 (networking): CLOSED for what QEMU can prove — loopback,
  SO_REUSEADDR, ICMP errors, half-close on the wire, pit-skew purge.
  HONEST SCOPE: AF_INET6/raw/AF_PACKET sockets do not exist; a caller
  gets a clean refusal at socket(), which is a truthful "no stack"
  answer, not a lie. Implementing IPv6 is a future arc, not a gap
  hidden behind a fake success.
- Tier 4 (/proc //sys //dev): CLOSED — real maps, /proc/sys, /proc/net,
  status fields, ns-correct identities, /dev listable, /dev/shm.
- Tier 5 (syscall families): CLOSED — xattr, POSIX timers,
  copy_file_range/sendfile/splice, sendmmsg/recvmmsg, SysV sem/msg,
  mknod, statfs truth; execveat/io_uring/bpf are authoritative-ENOSYS
  BY DESIGN with working fallbacks.
- Tier 6 (loader/auxv): CLOSED — real auxv creds, /etc/ld.so.cache.
  HONEST SCOPE: no vDSO; every clock_gettime is a real syscall. That is
  a PERFORMANCE item with a correct slow path, not a fidelity gap.

**Still owed, and why:**
- FS_BOOT_SELFTESTS boot with /ext + /vfat mounted (sees the new
  foreign-FS rename/statfs arms live) — one manual boot.
- EliteDesk hardware batch (CW_SANDBOX flip, xHCI EP0 re-test, slice-7
  non-root, real-HW resolv.conf rewrite) — needs the machine.
- Scheduler: woken-from-stop procs starve while the waker spins
  (29 ms vs 1.4 s) — perf, tracked in memory.
- VSC-PCTS2016 conformance email — user action.

### 2026-08-23 — TRUE vfork (4a233f1, lxposix 27/27; standing debt repaid)
The private-stack shim died: a NULL-child-stack share child now keeps
the parent's rsp (the copied syscall_regs already carry it — the fix is
deletion). The shim predated the slice-89 suspension fences and had
quietly broken the contract: child-written locals (the `err = errno`
exec channel) were invisible, child argument reads found zeros. Proven:
shared-frame write visibility, 100 ms suspension, exec with shared-frame
argv, errno write-back, posix_spawn both ways, raw NULL-stack clone.
**Test lesson (three "all terms true, AND false" runs): a vfork child's
function call at the clone call's depth overwrites the RA slot the
suspended parent RETs through — parent resumes past its own rax store.
Same on real Linux (why glibc vfork pops its RA into a register); raw
clone tests must keep the child call-free in inline asm.** Full chain
revalidated (cross-personality + defboot green); a CHROMIUM-flavour
boot re-validation rides the next browser-arc session.
