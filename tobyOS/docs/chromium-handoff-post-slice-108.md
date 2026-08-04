# Handoff: Chromium on tobyOS — post-slice-108

Read this file, then `docs/chromium-hypothesis-ledger.md` (slices 105–108 at
the tail) **before touching anything**. The memory topic `chromium-bringup.md`
carries the same history in compressed form. Baseline commit: `f14fd2a`.

Your job, in order:

1. **Root-cause the multi-process bootstrap flake.** It is reproduced,
   archived and localized — but NOT explained, and FIVE hypotheses are already
   dead. Details in §3.
2. **Then continue the perf roadmap** from §5, which has been re-aimed twice
   by measurement and now points somewhere specific.

---

## 1. State of the arc

Chromium 151 runs unmodified on tobyOS (Track B). The perf vehicle is
`chrome-headless-shell` in a TobyTK window (`programs/chromewin`), driven over
a CDP pipe, with frames delivered either by CDP screencast (reliable default)
or by reading chrome's viz shared bitmaps (faster, new in slice 107).

**Tier 3 (GPU) is CLOSED for chrome, with evidence — do not reopen it.**
Chrome's GL requires ANGLE, ANGLE's GL backend requires an X display, and both
shipped binaries register only the `headless` and `x11` Ozone platforms. A
bare DRM render node is not a road this chrome offers. Our DRM/Mesa/virgl
stack is real and proven (`GL_RENDERER: virgl`) and remains valuable for
*other* Linux GL programs — it is a Track-B capability result, not a chrome
frame-rate result. See `docs/chromium-tier3-gpu-design.md` (tail) and slice
106 in the ledger.

## 2. What slice 108 added (all committed in `f14fd2a`)

| file | what is in there |
|---|---|
| `include/tobyos/abi/abi.h` | `ABI_SYS_VIZFRAME_MAP` (188) + `struct abi_vizmap` |
| `src/mmap.c` | `vizframe_map()`; poll returns the region index; coarse hash + early exit (item 3, MEASUREMENT IN FLIGHT at handoff) |
| `src/syscall.c` | `VIZFRAME_MAP` dispatch |
| `src/isr.c` | `[pfpte]` fatal-fault PTE dump (the flake instrument) |
| `programs/chromewin/main.c` | read-only mapping + zero-copy blit |

**All of this is now committed** (slice 108) and measured — see §4. Item 3
(stride 64 + early exit) landed at **52.4 fps**, and the zero-copy mapping at
49.2, i.e. the copy removal measured as noise while the hashing reduction did
not. Nothing in this table is pending.

## 3. THE FLAKE — your first job

**Symptom.** Multi-process chrome (`CW_MP`, required for the viz path)
sometimes reaches `sessionId` and never bootstraps. The guest clock stays
healthy (e.g. 289 s of 420 s), so it is not a timeout or a wedge.

**Rate.** 3 of 4 full-length runs bootstrap; roughly 1 in 4 fail. (Slice 107
claimed 2 of 4 — retracted in 108; that tally counted a 150 s run that was
merely cut off at 69 s of guest clock.)

**What is ESTABLISHED, from an archived failing run**
(`logs/gpuperf_viz_anim.001.log`):

- **No renderer is ever spawned.** Successful runs `[execve-argv]` a renderer
  at ~8 s; the failing run spawns browser + gpu-process + utility and no
  renderer at all. No renderer → no page → no frames.
- Two forked children die at 15.2 s and 17.5 s at the **identical** address,
  both **pre-exec** (forked, never `execve`'d).
- The gpu-process **re-execs** at 29 s — it died and was restarted.
- The fatal fault is a **NULL dereference** — `cr2 = 0`, `err = 0x6`
  (user, write) — at `rip = libc.so.6 + 0xb1a66`, with `rdi = 0x01200011` =
  `CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD`, i.e. **glibc's `fork()`**.
- The new `[pfpte]` instrument confirms the shape: the rip's page is
  **PRESENT** (`phys=0x54383000`), and address 0 has **NO MAPPING**. So this
  is a genuine NULL pointer dereference by the program, not a paging failure.

**HYPOTHESES ALREADY TESTED AND REJECTED — do not re-run these:**

1. *The CoW-fork cap of 16 fired.* No — only 4 CoW forks in both the failing
   and succeeding runs (`[fork] allow chrome CoW fork #N`).
2. *A chrome child crashing in glibc's fork is the discriminator.* No — the
   same crash occurs in runs that go on to produce 15,390 frames. It is
   survivable; the question is why it is sometimes fatal to bootstrap.
3. *The ~4 GiB `PROT_NONE` probe storm.* Present in both (36 vs 37 probes).
4. *Failing runs never spawn `network`/`utility`/`none` children.* RETRACTED —
   that came from grepping `type=` across the whole log (which matches
   `--vmodule` text) instead of the `[execve-argv]` event, and from a log that
   was actually a **successful** run before per-run archiving existed.
5. *The forked child loses its TLS/FS base.* No — `sys_fork` does
   `memcpy(child, parent, sizeof(*child))`, so `tls_base` is inherited.
   (Note `sched.c:556` restores FS base only `if (to->tls_base)`; that is a
   latent smell worth a look, but it does not explain an inherited non-zero
   base.)

**Where to go next.** The open question is why glibc's `fork()` dereferences
NULL in the child. `THREAD_SELF`-shaped access at offset 0 is the obvious
suspect, so the decisive instrument is: at the fatal fault, print the process's
`tls_base` AND the live `IA32_FS_BASE` MSR (`rdmsr 0xC0000100`), plus whether
this proc is a fork child and of whom. If `tls_base` is non-zero but the MSR
reads 0, it is a restore/race bug on the child's first run; if both are fine,
the NULL is in glibc's own state (e.g. a pthread structure our fork did not
carry) and the next step is disassembling `libc.so.6 + 0xb1a66` in the staged
sysroot to see exactly which pointer it loads.

**Make it tell you before implementing anything.** That law has been paid for
repeatedly in this arc (slices 99–105, 106), and I broke it twice inside slice
108 alone — see §7.

## 4. MEASURED numbers (all steady-state, `anim.html`, SMP=4)

| configuration | fps | note |
|---|---|---|
| single-process CDP screencast | 42.6 | the long-standing baseline; unchanged by slices 97–105 |
| multi-process CDP screencast | 41.9 | multi-process costs nothing in fps… |
| multi-process viz shm (copy) | 48.8 | **+16.5%** over CDP — slice 107 |
| multi-process viz shm (zero-copy) | 49.2 | removing the 1.9 MiB/frame copy bought **nothing** |
| viz shm, zero-copy + coarse hash + early exit | **52.4** | **+25% end-to-end over CDP**; the per-poll HASH was the real cost |
| viz shm, 4 ms poll instead of 15 ms | 38.3 | **worse** — polling harder steals the CPU that produces frames |

…but multi-process drives the **guest clock at ~46 % of wall**, which the fps
column does not show. The page's own rAF runs at **61/s**, so at 52.4 we are
capturing 86 % of what the workload itself produces — the remaining headroom
is ~14 %, not the ~2.3x that slice 68 once projected.

**What these numbers mean together:** slice 68's ~2.3× estimate assumed the
JPEG encode was the bottleneck. It was not. Neither was the copy, and neither
was poll latency. The cost was the **work done per poll** — which is why
sampling less per poll is the change that finally moved it. Every intuition
about where the time went in this pipeline has been wrong at least once;
measure before you optimise, and measure again after.

## 5. The roadmap from here

1. **Fix the flake** (§3). Until then the viz path cannot be the default:
   chromewin only switches over after 5 real viz frames, so a failing run
   silently stays on CDP, which is correct but means the win is unreliable.
2. **The next suspect is the PRODUCER, not the consumer.** We now capture
   52.4 against a page producing 61/s — 86% of its own rate. Measure chrome's
   actual commit cadence before optimising further; do not assume the
   remaining ~14% is sitting there for the taking. If it is real, the poll
   loop is still the place it would come from (a signal instead of a poll).
3. **Then re-rank.** §6 of `docs/chromium-handoff-post-slice-91.md` still
   lists the other candidates. With tier 3 closed and viz shm delivered, the
   honest question is whether frame rate is still the right target at all, or
   whether responsiveness (input latency, navigation time) matters more.

Also open, unrelated and unclaimed: the ~230 ms `[bklmax]` residue is still
unattributed, and chrome's GL-init failure path storms ~4 GiB `PROT_NONE`
mmap/munmap probes that make the guest crawl (slice 106) — a real VMM cost
signal if that loop ever appears on a path that matters.

## 6. How to run things

```bash
bash logs/gpuperf.sh {cpu|gl|gle|gld|mp|viz} {anim|webgl}      # RUNSECS=<n> to shorten
```

- `cpu` = single-process CDP baseline. `mp` = multi-process CDP. `viz` =
  multi-process + read chrome's shared bitmaps. `gl*` = the closed GPU arms.
- Every run is archived as `logs/gpuperf_<mode>.NNN.log`; the plain
  `gpuperf_<mode>.log` is "latest". **This exists because overwriting the only
  failing run is what forced the slice-108 retraction.**
- **Gates are not optional.** Gate 1 = guest clock vs wall clock (catches
  freeze, panic and crawl at any run length; `mp`/`viz` get a lower bar
  because multi-process legitimately runs the guest slower). Gate 2 = in `gl`
  modes chrome must name `virgl`, else the fps is measuring the CPU path.
  The census carries its own vacuity guard (≥3 rounds after the first frame).
- Measure steady-state fps from the `[cwviz] frame N` / `[cwif] frame N` lines
  with their own timestamps, from ~25 s onward. Do not use the harness's
  `FRAMES(...)` line as an fps figure — it is the last counter value, not a
  rate, and the run's guest clock is shorter than its wall clock.
- `bash logs/defboot.sh` for the stock-path regression after any kernel change.

## 7. Method notes from slice 108 — read these, they were expensive

- **Grep the EVENT, not the string.** `type=renderer` appears in argv dumps and
  `--vmodule` arguments; `[execve-argv] … --type=renderer` is the event. The
  sloppy form invented a difference between two runs that was not there.
- **chromewin's stdout is chunked through the `[fd1]` logger**, so a line reads
  `[N ms] [fd1] len=13: [cwviz] frame ` with the NUMBER in a later chunk.
  Greps that expect `marker N` on one line silently find nothing — this cost a
  false "VIZFRAME_MAP never succeeded" and a false "produced NO frames".
- **A register dump can contain more than one fault.** I read an `err=0x14`
  instruction-fetch entry out of the `[pfres]` ring and called it the fatal
  fault; it was an earlier *resolved* one. The fatal fault was `cr2=0`. Print
  the PTE (`[pfpte]`) and believe that.
- **This env's bash heredocs EAT BACKSLASHES** — `printf '\003'`, C `"\n"`,
  Makefile continuations all corrupt. Use the Write/Edit tools for anything
  containing a backslash. I broke this law inside slice 108 and produced a
  Python guard with a literal newline in a string.
- `make` does NOT rebuild on `EXTRA_CFLAGS` change ⇒ `rm -f src/*.o` when
  switching flavour. `gpuperf.sh` already force-rebuilds `chromewin.o`.
- Builds and QEMU runs belong in SEPARATE shells, with runs prefixed by real
  `TMPDIR/TMP/TEMP` — a stray `TMP='C:\t'` reaching QEMU breaks `-snapshot`
  and produces silent no-log runs.
