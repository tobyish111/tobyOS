# Native-parity handoff: bringing tobyOS's own userland to Linux level

Written 2026-09-03 at the end of a two-day real-hardware validation pass.
This is the entry point for the next agent. Read it top to bottom before
touching anything; the traps at the end each cost a reboot cycle to learn.

---

## THE PROMPT

> tobyOS is a from-scratch x86-64 kernel at `c:\CustomOS\tobyOS`, branch
> `feat/posix-shell`, that already runs unmodified Linux userland: glibc
> binaries, bash 5.2, CPython, Chromium 151, containers under six namespaces
> with cgroup v2 and seccomp. Its own shell `tsh` is at bash parity by
> differential measurement (110/110 against GNU bash 5.2, 99.3% on the Oils
> POSIX suite). The Linux ABI side is strong.
>
> The gap is the other direction: **tobyOS's NATIVE userland is now less
> correct than the Linux userland it hosts.** Native `date` says 2025 while
> busybox prints the real date. Native `ps` lists services while the shell
> builtin lists processes. Native `strace` cannot trace native binaries.
> `/bin/sh` is a minimal shell that scores 0/14 against bash, and `system()`
> routes through it. Close that inversion, then close the remaining utility
> and stability gaps, until a user cannot tell tobyOS's own tools from the
> Linux ones by behaviour.
>
> Work through `docs/native-parity-handoff.md` in priority order. For every
> item: implement it fully, add gate cases that would have FAILED before the
> change, get the QEMU gates green, and then **confirm on the real EliteDesk
> through `logs/edremote.sh`**, because five of the last six bugs were
> invisible in QEMU. Commit each item separately with a message that says
> what was wrong, why it was wrong, and how it was proven. Update the memory
> files. Do not declare an item done on a green emulator run alone.

---

## The state you inherit

| measure | value | how to reproduce |
|---|---|---|
| bash parity (differential vs bash 5.2) | 110/110 | `bash logs/shparity.sh` (own boot, `-m 4096`) |
| interactive pty parity / job control | 4/4, 7/7 | `bash logs/ttyparity.sh` |
| Oils third-party POSIX suite | 1271/1280 | `bash logs/oilspec.sh` (own make, see trap) |
| distro-parity gate | 160/160 | `bash logs/distrotail.sh` (~12 min) |
| Linux POSIX gate | 32/32, enosys_gaps=0 | `bash logs/lxposix.sh [--full]` |
| utilities: native / busybox-only / absent | 268 / 0 / 7 | `python <scratchpad>/census.py` |
| sed / awk / grep vs the GNU tools | 88 / 129 / 56 | `bash logs/textgate.sh` |

Recent commits, newest first, all on real hardware evidence:

```
cae3c23 tool(edremote): find the box when DHCP moves it
6f62d0f fix(ps): /bin/ps had never worked -- wrong cap, and a hand-copied struct
a4ad8a9 fix(ptrace): ptrace_detach_all() existed, claimed to be called, and was not
3c523d2 fix(ptrace): ptrace_stop yielded under the BKL and wedged real hardware
a0e8c36 revert(ptrace): native syscall stops wedge real multi-core hardware
72db6e0 fix(rtc): /proc/stat wedged real hardware -- lock the CMOS, bound the wait
d61b929 feat(exec,proc): #! scripts, real /proc/stat, native strace, symbolic chmod
```

Memory files to read before starting (in `~/.claude/projects/c--CustomOS/memory/`):
`MEMORY.md` (the index), `shebang-procstat-native-strace.md` (this arc,
including two RETRACTIONS worth understanding), `elitedesk-remote-channel.md`,
`wake-after-death-flake.md`, `native-time-is-fake.md`, `tobyos-build-env.md`,
`shell-bash-parity-gate.md`, `distro-parity-utilities.md`.

---

## The work, in priority order

Ordered by what a user hits first. Each has a "proof" line: the test that
must exist and pass, on hardware, before the item is done.

> PROGRESS 2026-09-03 (8 commits on feat/posix-shell, ebb204f..f9ea44a):
> items 1 (clock), 2 (ps), 3 (strace), 4 (/bin/sh), 5 (ABI_ARG_MAX) and 8
> (2 of the oils groups -- 6 POSIX cases) are IMPLEMENTED, QEMU-GREEN and
> COMMITTED. `bash logs/distrotail.sh` 198/0, `bash logs/shparity.sh` 111/111,
> oilspec 6 POSIX cases fixed. Item 9's `logs/hwgate.sh` runner is written.
> A $$-is-a-forked-child's-pid bug was found and fixed en route (commit
> 8dab1e6). **HARDWARE PROOF IS OWED FOR EVERY ITEM**: the EliteDesk was
> powered off the whole session (edremote --check exited 90), so nothing was
> confirmed on real hardware -- run `bash logs/hwgate.sh` per section and
> `bash logs/edordinary.sh` once the box is up. STILL OPEN: item 6 (the
> intermittent wedge -- did NOT reproduce in QEMU this session), and the
> hardware proof for everything below.
>
> **2026-09-04 (QEMU-green, committed): item 7 is DONE** -- all 46
> busybox-only utilities are native (commits "feat(kernel): the syscalls and
> /proc files the native utilities were missing", "fix(tsh): 512 positional
> parameters...", "feat(libtoby): xz", "feat(userland): the 46 busybox-only
> utilities, natively", "test(gate): distro cases for the 46..."); the distro
> gate is 243/243, shparity 111/111. **Item 10's QEMU-doable part is done**
> (`/proc/vmstat` exists with a live page-fault counter); HDA, PS/2 and xHCI
> are hardware-only and untouched. **Item 8 moved:** POSIX-class Oils cases
> 1366, 1414, 1415 and 2373 pass now; what remains is 1069 (`ulimit -f`),
> 1286 (`type` -> alias/external) and three exec-status nuances -- the
> committed baseline is `logs/oilspec_failures.BASE_2026-09-04.txt` (compare
> SETS against it, never the last run's archive).
>
> Found on the way and FIXED: the module loader executed modules from the NX
> heap (hello.ko was the first module ever loaded); `su` accepted any password
> (native `getuid()` returns the EFFECTIVE uid, 0 after su's own setuid exec --
> `ABI_SYS_GETRUID` now); `useradd` never registered in the kernel login
> database, so its accounts could not log in, su or sudo (`ABI_SYS_USERADD`);
> libtoby `sscanf` had no scansets (sudo's parser matched nobody); a
> subshell's trailing redirections pointed into a dead stack frame. Found and
> NOT fixed -- see item 11.

### 1. Native programs have no real clock

**Wrong:** `libtoby/src/time.c` treats `time_t` as seconds since boot and
`gmtime_r` fakes a base of 2025-01-01, by its own comment. `date` on real
hardware printed `Wed Jan 1 00:01:11 UTC 2025` seventy-one seconds after a
boot whose RTC read 2026-09-01. Busybox prints the true date because the
Linux path's `lx_realtime_ns()` in `src/syscall.c` anchors CLOCK_REALTIME to
`rtc_unix_time()` once and drives it from the TSC.

**Root gap:** the native ABI has no realtime syscall. Its only clock is
`ABI_SYS_CLOCK_MS` (48), milliseconds since boot.

**Do:** add a native realtime syscall (or expose the vdso's `epoch_base_ns`
to the native path; either is fine, one number per boot). Make libtoby's
`time()`, `gettimeofday()`, `clock_gettime(CLOCK_REALTIME)` use it. Rewrite
`gmtime_r`, `localtime_r`, `mktime`, `strftime` against the 1970 epoch. Make
`stat` times and tobyfs inode stamps agree with it (slice 6 already made
tobyfs stamp from the Linux clock; confirm native reads the same numbers).

**Watch:** `src/rtc.c` was fixed on 2026-09-02 to lock the CMOS and bound the
update wait. Do not add another RTC read to a hot path; read it ONCE at boot
and cache, as `/proc/stat`'s btime now does.

**Proof:** on hardware, native `date` and `busybox date` agree to the second;
a file touched by a native program and stat'd by busybox shows a 2026 mtime;
`printf '%(%Y)T' -1` in tsh prints 2026. Gate case in `etc/distro_test.sh`
with the year asserted against `btime` from `/proc/stat`.

### 2. `ps` is two different things under one name

**Wrong:** `/bin/ps` (`programs/user_ps`) lists SERVICES from
`ABI_SYS_SVC_LIST`; the shell's `ps` builtin dumps the real process table.
On Linux `ps` lists processes. The binary was rewritten on 2026-09-02 to
actually work, but it still answers the wrong question.

**Do:** make `/bin/ps` a real procps-style tool reading `/proc/<pid>/stat`
and `/proc/<pid>/status` (both exist; the Linux slices built them), with
`ps`, `ps aux`, `ps -ef`, `ps -o pid,comm,%cpu,%mem,etime,cmd`. Move the
service listing to `svcctl list` (svcctl already exists). Keep the shell
builtin in agreement or remove it.

**Proof:** `ps aux | grep tsh` finds the running shell on hardware; the
column layout matches procps closely enough that a script parsing `ps -o
pid= -C name` works. Gate case comparing `ps` pid set against `/proc`.

### 3. Re-enable native-path syscall stops for `strace`

**History, read carefully:** enabled 2026-09-01, wedged real hardware,
reverted 2026-09-02 (`a0e8c36`). Then the REAL cause was found: the shared
`ptrace_stop()` in `src/ptrace.c` yielded while holding the BKL, which
affected the Linux path too. That is fixed (`3c523d2`) and proven with 80
cumulative traces on hardware. The native stops were left reverted
deliberately -- one change at a time.

**Do:** restore the two `ptrace_syscall_stop(caller, ...)` calls around
`rv = do_syscall(...)` in the native branch of the dispatcher in
`src/syscall.c` (the long comment there records both attempts; replace it
with the true story). Remove the refusal in `programs/strace/main.c`.
Restore the gate assertions that trace `/etc/dbg_test.elf`.

**Proof:** `strace /etc/dbg_test.elf` on HARDWARE names its syscalls from
the native table and reports `exited with 7`; then 40 such traces in a row
with progress written to `/data`, box healthy afterwards. If it wedges, the
answer is not "revert" again -- it is a second mechanism; find it with the
`/data` technique.

### 4. `/bin/sh` is the minimal shell

**Wrong:** `programs/user_sh` scored 0/14 against bash and was left
"untouched" when tsh was built (Makefile ~3137). `system()`, `popen()`, and
`#!/bin/sh` scripts route through it. libtoby's `execvp` ENOEXEC fallback
was pointed at `/bin/tsh` explicitly to dodge this.

**Do:** make `/bin/sh` be tsh (a hard link in the initrd tar list, plus tsh
noticing `argv[0]` ends in `sh` and running in POSIX mode -- bash does
exactly this). Point `system()`/`popen()` at `/bin/sh` per POSIX now that it
is trustworthy. Retire or archive `user_sh`.

**Proof:** `sh -c 'echo $((1+2))'`, `system("ls | wc -l")` from a native C
program, and a `#!/bin/sh` script using `[[`-free POSIX constructs all work
on hardware. Run `logs/shparity.sh` and `logs/oilspec.sh` afterwards --
POSIX mode must not regress bash mode.

### 5. Long SSH exec commands are silently truncated

**Wrong:** `src/ssh.c` holds an exec command in 4096 bytes and REFUSES
longer ones rather than truncating -- good. But it hands the command to
`tsh -c <string>` as one argv element, and the spawn path caps an element at
`ABI_ARG_MAX` = 512. So 512-4095 bytes are cut silently and produce nothing.
Measured: 500 bytes fine, 660 silent. `logs/edremote.sh` works around it by
uploading over stdin.

**Do:** either raise `ABI_ARG_MAX` (audit every `kargv_pool`-style fixed
buffer that sizes off it first -- `src/fork.c` has a static
`[ABI_ARGV_MAX][ABI_ARG_MAX]` pool), or have ssh.c write a command longer
than `ABI_ARG_MAX` to a temp file and run `tsh <file>`. The second is
smaller and safer. Either way, the SSH server's refusal threshold must equal
the real one, so it never lies.

**Proof:** `ssh toby@box "$(printf 'echo x; %.0s' $(seq 400))echo END"`
prints END on hardware. Remove the stdin workaround's threshold from
edremote.sh only after this lands.

### 6. The intermittent wedge on real hardware (PARTLY SOLVED 2026-09-08)

**One half of this is solved, reproduced and gated.** The shape recorded
below -- the box answers ping, completes TCP handshakes, and no service
ever speaks (`edremote --check` returns 92) -- is what a HELD SSH SESSION
SLOT looks like, and there was a deterministic way to hold it: the server
acknowledged `SSH_MSG_CHANNEL_EOF` and threw it away, so edremote's own
`ssh host 'cat > file' < local` upload left `cat` on a stdin that never
ended. Measured in QEMU at 33 minutes with the guest otherwise healthy.
Short commands never redirect stdin, which is exactly why only uploads
ever wedged it. Fixed, with two more bugs behind it (the pty input ring
dropped bytes and reported success; an exec channel ran through a terminal
line discipline that truncated a large upload at 4095 bytes), and gated:
`bash logs/sshreap.sh`, 28/28, over SLIRP hostfwd with the host's own
OpenSSH client. See `ssh-session-slot-wedge` in memory.

**What is NOT settled:** whether the two remaining sightings below were
this bug. They were a GUI editor session (day 1) and a plain `chmod`/`stat`
line (day 3), neither of which uploads anything -- but both were observed
THROUGH this same channel, where a held slot is indistinguishable from a
kernel stall. Re-test on hardware before assuming either way, and note
that `logs/hwgate.sh` should be runnable now: its 21 KB script upload was
failing its md5 check for the reason above.


**Facts:** a state where the box answers ping and completes TCP handshakes
(NIC interrupt path) but no service ever speaks (pid 0's
`net_service_tick` lane). `bash logs/edremote.sh --check` returns 92 for it.
Five specific causes were found and fixed this week; **one remains**, seen
during a GUI editor session (day 1) and during a plain `chmod`/`stat` line
(day 3) on the fully-fixed build. It is intermittent and reached through
ordinary work. `wake-after-death-flake.md` has every measurement.

**Do NOT:** guess. Four hypotheses died to measurement this week (abandoned
session, session churn, spinning process, ps). Each was plausible.

**Do:** an instrumented long-running loop of ORDINARY shell work (file
create/chmod/stat/rm, small pipelines, a spawn or two) with every iteration
stamped to `/data/<name>.txt`, left running for an hour over the channel.
When it wedges, `/data` says which iteration and which command. Build
`-DSMP_DIAG` so `bkl_owner` is named rather than `-2`. Then read the serial
console for the `[kpf]` line, not the register block (see the flake memo).
Only with that evidence, theorise.

**Proof:** the loop runs for an hour without a 92; or the wedge is
root-caused with a commit that names the mechanism and a reproduction that
went from "fires" to "does not fire".

### 7. The 46 busybox-only utilities, natively

The original ask was every common utility "part of the tsh shell language".
These work today through busybox and are not native. Exact list from the
census, grouped; many are an afternoon each and several are trivial:

| group | names |
|---|---|
| coreutils (17) | `arch chgrp factor false link logname nproc printf pwd shred sum test true unlink uptime users who` |
| archive (5) | `ar cpio unxz xz xzcat` |
| text (1) | `ed` |
| process (6) | `crontab init insmod lsmod modprobe rmmod` |
| filesystem (6) | `blkid fsck mkfs.ext2 mkfs.minix mkfs.vfat tune2fs` |
| network (8) | `arp arping hostid ip netstat route telnet traceroute` |
| accounts (2) | `passwd su` |

Notes: `true false pwd printf test` exist as tsh builtins; what is missing is
the `/bin` binary bash also ships. `insmod/lsmod/modprobe/rmmod` need a
module loader -- `[module] /lib/modules not found` at boot says the subsystem
exists and is empty; decide whether that is in scope. `xz` is a from-scratch
LZMA2 decoder like the zstd one that already exists. `ip` and `netstat` have
the netlink and `/proc/net` sources already built.

**Proof:** census shows 0 busybox-only in each group closed; each new tool
has distro-gate cases asserting output the busybox one produces, run on
hardware.

### 8. The Oils POSIX suite: 9 named failures

1271/1280. The nine are pre-existing and unexplained. `shell-oilspec-
third-party-gate.md` has the trap: the report diffs against the LAST RUN'S
archive, not a baseline, and the failures file is gitignored -- follow the
clearing recipe there before believing a diff.

**Proof:** 1280/1280, with the archive committed as the baseline this time.

### 9. Real hardware as part of the gate, not an afterthought

Every bug this week was QEMU-green. `logs/edremote.sh` exists, finds the box
by its SSH banner when DHCP moves it, and returns distinct codes for off /
shut / wedged. `logs/edchurn.sh` is a stress harness.

**Do:** a `logs/hwgate.sh` that runs a chosen subset of `etc/*_test.sh` ON
THE BOX through edremote, with progress to `/data`, and reports pass/fail
per section the way distrotail does. Run it after every ISO.

**Proof:** one command, hardware verdict, no serial cable.

### 10. Real-hardware device gaps (lower priority, EliteDesk-specific)

- HDA audio finds 0 codecs: `verb timeout` then `codec 0: no Vendor ID
  (resp=0xffffffff)`. Second controller at 00:1b.0 is ignored by design.
- PS/2 mouse init NACKs every command; harmless because USB HID binds.
- xHCI ports 5 and 6 (`speed=1`) never enable after reset.
- `procs_blocked` in `/proc/stat` is 0 by design (no D-state). `/proc/vmstat`
  exists since 2026-09-04 (nr_free_pages, pgfault, pgmajfault, pswpin/out).

Absent and out of scope unless asked: `node`, `perl`, `ruby` (ports);
`vim` (tvi exists; the name would misname it); `gcc` (`cc` is tcc and works).

### 11. The native text tools are subsets -- CLOSED 2026-09-06 (QEMU)

**What it said:** awk was a 313-line subset (no `$NF`, no `?:`, no
`sub()`/`gsub()`, and a `$1=="x"` pattern did not select); a sed bracket
expression returned the whole rest of the line instead of the capture;
grep had no `-m`.

**What it actually was.** The sed complaint was not about brackets. The
regex engine's parser handed fragments around as a single entry pointer and
rediscovered the other end by walking `->out` (`chain_end`), which works
only while the graph is a tree -- and a quantifier makes it a GRAPH. For
`\([0-9.]*\)` that walk went round the star's own loop, tripped its cycle
guard, and then **overwrote a loop edge with the group-end pointer**. The
star was destroyed, the fragment's real exit was orphaned with a NULL
successor, and an orphaned exit node is read by the matcher as "accept
here" -- so the zero-iteration branch accepted the whole pattern and the
group was never closed. **Every group whose last element is quantified was
miscompiled**, `(a*)` and `\(x\)*` alike, BRE and ERE.

**Done:**

- `libtoby/src/regex.c` rebuilt around an explicit `frag_t {entry, exit}`.
  Every construct wires its own exit; `chain_end` is gone. Also added:
  POSIX `[:classes:]` (`[[:alpha:]]` used to parse one byte at a time and
  match the set `[ : a l p h`), `\b \B \< \>`, BRE positional `^`/`$`
  with `+ ? ) |` literal and GNU's `\+ \?`, a literal `{` that does not
  open an interval, leftmost-LONGEST for alternation (`grep -oE 'a|ab'`),
  and `REG_ESCBRACKET` -- a private flag for awk, because awk processes
  backslash escapes inside a bracket and sed and grep provably do not.
  A greedy quantifier over a single-node body compiles to one iterative
  `ND_REPEAT` instead of recursing once per character consumed: a 4 KiB sed
  line used to come within a hair of the bottom of a 256 KiB user stack.
- `programs/awk/main.c` is a real POSIX awk (lexer, recursive-descent
  parser, tree-walking interpreter): user functions with arrays passed by
  reference, all four getline forms, the three print redirections, every
  built-in, and the POSIX comparison rule. Two value-semantics bugs found
  by the differential suite and fixed: a cached number-to-string conversion
  used to change a value's TYPE (`x=2; s=x ""` then `x<10` was false), and
  `truth()` consulted a cached numeric value so `x="0abc"; y=x+0` made x
  false.
- `ports/sbase/grep.c` gained `-m`, `-e`, `-f`, `-A/-B/-C` with GNU's `--`
  group separator, and `-s`. `-o` printed only the FIRST match per line;
  `-w`/`-x` gave up at the first candidate instead of scanning on. And
  **`grep 'x$'` had never matched anything**: fgets keeps the newline, the
  pattern was matched against the whole buffer, so `$` sat one character
  short. It reported "no lines", never an error, which is why it lasted.

**Proof:** `bash logs/textgate.sh` -- one boot, three fixtures
(`etc/sed_test.sh`, `etc/awk_test.sh`, `etc/grep_test.sh`), each run twice:
by tsh in the guest and by bash on the host against real GNU sed/awk/grep,
diffed section by section. The section list is derived from the fixture, so
a new case is graded the moment it is added. 27 rx_ sections were added to
the sed fixture; **21 of them fail against the pre-rewrite engine**, which
is what makes them a gate rather than a description.

Because libtoby's regex.c, sed, awk and grep all build against host gcc
unchanged, the same fixtures run on the host in seconds -- do that before
paying for a boot.

**Still open here:** `ls -l` prints a synthetic link count (1 for files, 2
for directories) and `test -ef` is path equality, both because libtoby's
`struct stat` carries no `st_nlink`, `st_ino` or `st_dev` and
`struct abi_stat` has nowhere to put them. That is an ABI change, not a
text-tool one, and is the last piece of this item.

**HARDWARE PROOF OWED**, as for every item above it.

---

## Laws

**Build.** From git-bash:
```
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t
make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" iso
```
The `TMP='C:\t'` prefix ON the compiler variable is what makes `.S` files
and host tools build; `export TMP` before make does not survive MSYS.

**Gate flags are not files.** `EXTRA_CFLAGS` changes do not trigger
recompiles. `rm -f src/kernel.o src/sched.o src/thread.o` before any build
whose flags differ from the last one, or you ship the previous gate's kernel.
A plain user ISO is `make iso` with default flags after that `rm`.

**One boot at a time.** Never two gates. Never rebuild the ISO while a QEMU
is running from it. Edit sources only once QEMU is up (ISO baked).

**Every gate greps `@@marker` sections from a serial log**, and
`kern_strip` drops every line beginning with `[`. A tool whose output starts
with `[` (gdb's `[Inferior 1 ...]`) is invisible to a `ck`/`eq` check; count
it in the guest with `grep -c` and assert the count.

**Hardware.** `bash logs/edremote.sh 'cmd'` runs on the EliteDesk; `-f file`
uploads a script; `--check` preflights. Exit 90 off, 91 port shut, 92
**wedged**. It finds the box by banner if DHCP moved it. Rules:
- **Every hardware test writes progress to `/data`** (the USB stick). SSH
  output dies with the box; the file survives the wedge and the reboot.
  This single technique overturned four wrong hypotheses this week.
- Batch work into one session. Use `ED_TIMEOUT`, never an external kill.
- A green QEMU gate is not evidence about parallelism or real devices.

**Commits.** One item per commit. Say what was wrong, why, and the proof.
End with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
`logs/` is gitignored: new harness files need `git add -f`.

---

## Traps that each cost a cycle

- **The Bash tool mangles heredocs** containing single quotes or `\n`
  escapes -- three patches silently did nothing. Write patch scripts with
  the Write tool, then `python script.py`. The scratchpad `patchlib.py`
  (`load`/`save`/`sub`) handles CRLF files; `src/proc.c` is CRLF.
- **A backgrounded Bash command resets the session cwd** to `c:\CustomOS`.
  `cd /c/CustomOS/tobyOS` at the start of every command.
- **`vfs_stat.mode == 0` means "no permission info"**, not 0000.
- **An anchor that appears twice** (e.g. the two group-teardown loops in
  `src/proc.c`) makes a one-shot `sub` fail or, worse, patch half. Count.
- **The CMOS is one index/data port pair** with no per-CPU state; any new
  RTC read must take `g_cmos_lock` in `src/rtc.c`.
- **Yielding under the BKL stops the machine.** Any new blocking point in a
  syscall must use the `bool had = bkl_held(); if (had) bkl_exit(); ...;
  if (had) bkl_enter();` idiom.
- **Dead code that says it is called.** `ptrace_detach_all` had a comment
  claiming it ran from every exit path and nothing called it. The grep for
  functions referenced only by their definition (see `dead-subsystem-audit`)
  is worth re-running.
- **The box moves.** DHCP gave it .90 for a day and .101 the next. The tool
  handles it; a hardcoded address in a new script will not.

---

## Definition of done

A native tobyOS user session is indistinguishable in behaviour from the
Linux userland the kernel hosts: same date, same `ps`, `strace` on
anything, `/bin/sh` that is a real shell, no utility that silently
hands off to busybox, the Oils suite at 1280/1280 -- and every claim
proven by a gate that runs on the real EliteDesk, with an hour of ordinary
work producing no exit-92.
