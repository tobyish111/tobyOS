# Chromium on tobyOS — handoff to a deep-reasoning agent (Fable 5)

You are picking up a **live, successful** Chromium bring-up on tobyOS at three
independent walls. Real, unmodified `chrome-headless-shell` **151.0.7922.34**
already runs its full multi-process engine (browser + in-process GPU + network
service + utility + renderers; Mojo/ipcz, cross-process shared memory,
SCM_RIGHTS, futex/eventfd) on tobyOS's Linux ABI (Track B) and **navigates to a
live web page over HTTP**, printing the parsed DOM with `--dump-dom`, exit 0.

Your mission: get it to **"visit any page like a normal browser"** — which means
solving, in whatever order you judge best, three deep and *independent* fronts:

1. **HTTPS** — blocked by chrome's post-quantum key exchange (ML-KEM) failing on tobyOS.
2. **Pixels / rendering** — `--screenshot` crashes in SwiftShader GL init (NULL deref).
3. **Window + input integration** — not started; blocked on (2).

Each is genuinely hard. This document gives you the exact state, the proven
diagnosis, what has been ruled out (do not re-run these), the tooling, and a
concrete attack plan per front. **Read the two ledger sections named below in
full before touching anything.**

---

## 0. Prime directives (learned the hard way this arc)

- **Get the real error before theorizing.** Two full sessions were lost to
  plausible-but-wrong hypotheses (a "cert-verifier hang", a "cert-policy/CT
  rejection", an "AES-GCM bug", a "SIMD-crypto bug"). Every one was killed by
  reading chrome's **own NetLog** (`--log-net-log`, then the kernel scans the
  file). Reason from the logged error code, not the symptom.
- **After changing `struct proc` or any widely-embedded header, delete ALL
  kernel `.o` and rebuild** before trusting a single observation. Stale-object
  layout corruption produces *convincing, stable, false* application-layer
  symptoms (this cost a wrong documented conclusion once):
  `find . -maxdepth 3 -name '*.o' | grep -vE 'programs/|libtoby|sdk' | xargs rm -f`
- **The tobyOS stack itself is sound.** Its own TLS 1.3 client fetches the same
  HTTPS URL that chrome fails on (validates the real cert against the real
  clock). The kernel, sockets, TCP, crypto-in-C, DNS, and route all work. The
  remaining walls are **chrome-internal** (inside an off-the-shelf binary you
  cannot rebuild) or in tobyOS's GL/graphics support.
- `.argc` in the chrome spawn harness (`src/kernel.c`) is **hardcoded** next to a
  NULL-terminated `argv[]`. Re-count every time you touch the argv. A stale count
  makes `proc_spawn` read past the terminator and misreport "binary not present".
- `last_fault_rip` / `fault_count` in the isr dump are **cumulative latches**,
  not the cause of death. Use the **register-dump `rip`** and `cr2` from the
  `EXCEPTION 14` block.

---

## 1. Read these first (in order)

| Resource | Why |
|---|---|
| `docs/chromium-hypothesis-ledger.md` **slices 34–37** (the tail) | The complete, current diagnosis of all three walls with evidence. **Most important file.** |
| Memory: `chromium-bringup.md` (esp. the SLICE 35-36 section) + `MEMORY.md` index lines for "Chromium bring-up" and "Chrome-parity push" | Cross-session summary + all the traps. Path: `C:\Users\tdude\.claude\projects\c--CustomOS\memory\` |
| `docs/chromium-render-gl-bug-prompt.md` | The render/GL wall, written *for exactly this crash*. Lists what's already been tried (fake X server, Vulkan loader, WSI) — do not repeat. |
| `docs/chromium-bringup-m1.md` (slices 11–23) + `docs/chromium-bringup-m0.md` | Full burn-down of the earlier walls; the GL/SwiftShader deep-dive is in m1 slice 20. |
| `git log --oneline b9b7efc..HEAD` | The last 5 commits are this arc: `b9b7efc` DOM renders, `ceec229` real-URL nav, `5011817` HTTPS root cause, `27a1ca6` render tier. Read the commit bodies. |

---

## 2. How to build, boot, and iterate

**Environment:** Windows host; cross toolchain + `make` live on the MSYS2 PATH;
QEMU is TCG (software emulation, no KVM on Windows) — correct but slow (~2–3 min
build, chrome runs ~30 s of wall-clock reaching the DOM).

**The chrome harness** lives behind `#ifdef CHROMIUM_BOOT` in
`src/kernel.c` (search `Track B M0: spawn a REAL`). It: (a) runs three unit
tests (linux-futex / linux-eventfd / linux-mapshare — primitives under Mojo),
(b) runs a native-TLS `[https-probe]`, (c) spawns chrome with a hardcoded
`argv[]`/`envp[]` + `.argc`, (d) after chrome exits, scans `/data/netlog.json`
(`[netlog]`) and checks `/data/shot.png` (`[screenshot]`).

**Canonical run script:** `logs/chromium-m0.sh` (build ISO with
`-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT`, boot headless QEMU, grep the gap
list). Set `TRACE=1` for the `-DLINUX_SYSCALL_TRACE` firehose (every Linux
syscall). Payload prereq: `bash programs/chromium/build.sh` (already staged:
`programs/chromium/chrome-headless-shell-linux64/` + `sysroot/`).

**Minimal iterate loop** (what this session used — write your own):
```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"; mkdir -p /c/t
export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
touch src/kernel.c src/syscall.c; rm -f build/initrd.tar build/base.iso tobyOS.iso
make "CC=TMP='C:\\t' TEMP='C:\\t' clang" "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
     iso EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT"
"/c/Program Files/qemu/qemu-system-x86_64.exe" -cdrom tobyOS.iso \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough -boot d \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -smp 4 -m 4096 -cpu qemu64,+smep,+smap -serial "file:logs/run.log" -no-reboot -display none &
# wait for: grep -aq 'M0 gap-list run complete' logs/run.log
```
Trap: the boot loop overwrites the log; a mid-boot read shows only early
"Loading initrd" — wait for the run's own completion marker. `-cpu qemu64` has
**no** AES-NI / PCLMUL / AVX / RDRAND / SSSE3 (SSE2 only). To validate you didn't
regress the base OS after kernel changes, boot **without** `-DCHROMIUM_BOOT`
(`logs/defboot.sh`) — must reach desktop+login, zero faults.

**Permanent in-tree instruments** (all `CHROMIUM_BOOT`-gated; keep + extend):
- `[https-probe]` — kernel-side native TLS 1.3 GET of https://example.com before
  chrome spawns; SUCCESS 200 proves the tobyOS net+crypto stack is the not the differ.
- `[tcp] WIRE` / `[tls] rx` / `[tls] TX` / `[tcp] connect` — TLS wire+record trace (`src/tcp.c`).
- `[netlog]` — after chrome exits, kernel `vfs_read_all`s `/data/netlog.json` and
  scans it for `net_error` + SSL reason context (`src/kernel.c`, after `proc_wait`).
- `[screenshot]` — checks `/data/shot.png` size + PNG magic.
- `[libmap]` (`base len prot off fd ino`) — every `.so`/segment mapping, so a
  crash rip → base+off → `objdump` the `.so`. `[amap]` — every mmap/munmap arg.
- isr.c dumps the full register frame + `cr2` on any user fault.
- `bt_dump_group(tgid)` / `bt_dump_one` (`src/syscall.c`) — walk a proc's user
  stack for code-region return addresses; fires from `signal_send` on SIGKILL of
  an `is_renderer` proc. Timer-gated dumps are unreliable (per-CPU TSCs unsynced
  under `-smp 4`).

---

## 3. FRONT A — HTTPS (highest leverage: gates ~the entire 2026 web)

### Diagnosis (proven, `5011817`, ledger slice 36)
Chrome negotiates the **post-quantum hybrid key exchange `X25519MLKEM768`**
(TLS group `0x11EC` = 4588) with Cloudflare/Google/most large sites. **BoringSSL's
ML-KEM path fails on tobyOS** → `ERR_SSL_PROTOCOL_ERROR` (-107), `ssl_error:1`
at `s3_pkt.cc` (record layer). Read straight out of chrome's NetLog.

**Why only ML-KEM fails while X25519 + the AEAD work:** ML-KEM decapsulation
ends in the Fujisaki–Okamoto re-encryption **equality check** — any single-bit
error anywhere in its thousands of mod-3329 / NTT / Keccak ops silently returns a
*pseudo-random* shared secret → the record layer can't decrypt the server flight
→ SSL_ERROR_SSL. X25519 (one tolerant scalar mult) and the record AEAD (once keys
are right) do **not** amplify a tiny error this way. This is exactly why tobyOS's
own plain-X25519 TLS 1.3 fetches the same URL (200) and chrome cannot.

### Ruled OUT — do NOT re-test these (each cost a build cycle)
- Transport (`[tcp] WIRE`: `tx… rx=4776 read=4776 ooo=0 retx=0` — byte-perfect).
- Cert date / trust / CT (`--ignore-certificate-errors` unchanged; native TLS
  validates the *real* cert against the *real* 2026-07-23 clock and gets 200;
  the error is -107, not a -2xx cert code).
- The AEAD cipher (`--cipher-suite-denylist=0x1301,0x1302` → forced ChaCha, identical fail).
- TLS version (`--ssl-version-max=tls1.2`, identical fail).
- Crypto implementation asm-vs-C (`OPENSSL_ia32cap=0`, identical fail).
- RNG (getrandom is fixed and a weak seed can't cause a *wrong* KEM result).
- Unzeroed memory (anon demand pages **are** `memset`-0, `page_fault.c:252`).
- Stack overflow (would SIGSEGV; chrome exits 0).
- **External disable-levers don't work in headless-shell:** `--disable-features=`
  `X25519MLKEM768,PostQuantumKyber,X25519Kyber768Draft00` does NOT reach the
  network-service child (only the field-trial `PaintHolding` override did); the
  managed policy `PostQuantumKeyAgreementEnabled:false` (staged in
  `/etc/{opt/chrome,chromium,opt/chrome_for_testing}/policies/managed/`) is
  **ignored** — chrome-headless-shell has no enterprise-policy machinery.

### Attack plan (pick a lane)
- **A1 (best if it works): make BoringSSL's ML-KEM produce a correct result on
  tobyOS/TCG.** It's pure C (no CPU dispatch on `qemu64`), so a *correct* TCG
  should compute it right. Suspect a subtle **memory** issue in ML-KEM's specific
  allocation/stack pattern, or a TCG instruction edge case. Concrete probes:
  (a) instrument tobyOS to dump the ML-KEM client key_share bytes the client
  *sends* vs the server key_share it *receives* (in `[tls]`), and whether the
  derived handshake secret matches what a reference decaps of the same inputs
  produces — run the *same* ciphertext through tobyOS's own crypto if you add an
  ML-KEM ref, or through a host BoringSSL, to see if tobyOS's execution diverges.
  (b) Check chrome's network-service **thread stack size** vs ML-KEM's stack
  footprint (tens of KB) — confirm tobyOS honors the pthread-requested stack and
  demand-pages clone stacks fully (not just the 8 MiB main grow-down from slice
  34). (c) Try `-cpu Nehalem`/`-cpu max` (adds SSE4/AES/etc.) to see if a
  different instruction mix changes the result — if it does, it's a TCG/feature
  edge, not memory.
- **A2 (pragmatic unblock): prevent the PQ group from being offered.** The flags/
  policy failed, but a beefier agent could: patch chrome's built-in field-trial
  testing config, or find the *current* feature name (this is a 2026 build; the
  name likely changed post-launch), or inject an `--enable-features`/group
  restriction that reaches the network service. If chrome falls back to plain
  X25519, HTTPS works immediately (tobyOS handles X25519 — proven).
- **A3 (last resort): test against a PQ-*disabled* server** to confirm the rest of
  the HTTPS path is clean, then focus A1. (example.com via Cloudflare offers PQ;
  you'd need a server that doesn't.)

**Definition of done:** chrome `--dump-dom https://example.com/` prints the real
page body (not `<html><head></head><body></body></html>`), exit 0.

---

## 4. FRONT B — Pixels / rendering (prerequisite for the window tier)

### Diagnosis (proven, `27a1ca6`, ledger slice 37)
`--screenshot` forces the GPU/viz compositor, which loads SwiftShader
(`libvk_swiftshader.so`) even under `--disable-gpu`, and **deterministically
NULL-derefs in GL init**: `EXCEPTION 14`, `cr2=0x0000000000000308`, `err=0x4`
(user READ of a not-present page) = reading field **`+0x308` off a NULL pointer**.
Faulting rip `0x100000b5270d` = **`libc+0x8d70d`** (libc base `0x100000ac5000`
from `[libmap]`). A NULL propagated from a failed SwiftShader/Vulkan init into a
libc call. Same class as the documented render-gl-bug wall; **unchanged by the
slice-34 CoW fix**. Identical fault under both `--disable-gpu`+`--screenshot` and
the proper `--use-angle=swiftshader --use-gl=angle` config → it's a real crash,
not a misconfiguration.

CPU-only software raster does **not** satisfy `--screenshot` in headless-shell
(it still wants a GL context) — so you cannot route around SwiftShader; you must
make its Vulkan init succeed.

### Attack plan
- **B1: symbolize the NULL-returning call.** `objdump -d
  programs/chromium/chrome-headless-shell-linux64/…/libc.so.6` around offset
  `0x8d70d`; identify the function and the `mov …0x308(%reg)` that faults; walk
  back to find which SwiftShader/Vulkan call returned NULL (via `bt_dump_one` on
  the faulting proc, and the `[libmap]` for the `libvk_swiftshader.so` /
  `libvulkan.so.1` / `libEGL.so` region). The render-gl-bug-prompt.md documents
  the chain: ANGLE Vulkan → `DisplayVkXcb::initialize` → `xcb_connect` to the
  in-kernel fake X server (`DISPLAY=:0`, `sock_unix_connect_named` for
  `/tmp/.X11-unix/X0`) → WSI extension check. Find which step yields NULL.
- **B2: the ICD / WSI path.** Env is set (`VK_ICD_FILENAMES=/opt/chrome/`
  `vk_swiftshader_icd.json`, `DISPLAY=:0`). Confirm the loader finds the ICD, the
  fake X server answers `xcb_connect`, and `vkCreateInstance` +
  `vkEnumeratePhysicalDevices` return non-NULL. Add `VK_LOADER_DEBUG=all` to
  `envp` to narrate loader discovery. The slice-12–14 fake-X-server + path-
  normalization work is in-tree and history.
- **B3: watch for the *other* SwiftShader crash** (slice 20): a flaky
  `memcpy+0x35d` write into a thread-stack guard page during Vulkan **extension
  enumeration** (corrupt/too-large length). If B1/B2 get past the NULL deref you
  may hit this next; it's a separate memory-corruption bug.

**Definition of done:** `[screenshot] /data/shot.png = N bytes, PNG magic OK`,
and the PNG visually matches the same chrome build on the host for
`http://example.com/`.

---

## 5. FRONT C — Window + input integration (blocked on B)

Not started. Today chrome is a **boot-time harness** that runs one hardcoded URL
and exits; there is no window, no omnibox, no input routing. Once B produces a
framebuffer/PNG per navigation, the work is:
- Run chrome as a long-lived process driven over the **DevTools protocol**
  (`--remote-debugging-pipe` — fds, no TCP needed) instead of one-shot
  `--dump-dom`, so a tobyOS front-end can send `Page.navigate` and receive
  `Page.captureScreenshot`.
- Present the screenshots in a **tobyOS GUI window** (the native toolkit is
  `libtoby`/TobyTK; see the memory "GUI framework + TobyTK" and the existing
  `programs/user_gui_browser/` native browser for the window/omnibox/input
  patterns to reuse).
- Route keyboard/mouse from the tobyOS window back into chrome via DevTools
  `Input.dispatchKeyEvent` / `Input.dispatchMouseEvent`.
This is a real integration project, not a bug-fix; scope it after B works.

---

## 6. Scope note on "like YouTube" (set expectations)
Beyond A+B+C, YouTube specifically needs H.264/VP9/AV1 **video decode** wired to
chrome's media stack and clears the **signature-cipher wall** that is *parked
even in tobyOS's native browser* (see memory "Chrome-parity push": YouTube video
PARKED). Treat "load and render a normal HTTPS page" as the milestone; full
YouTube playback is a further, separate arc.

---

## 7. One-paragraph status to hold in your head
HTTP navigation works and is committed. HTTPS is blocked *only* by chrome's
post-quantum ML-KEM failing on tobyOS (everything else in the HTTPS path is
proven correct by the native TLS probe). Pixels are blocked by a deterministic
NULL deref in SwiftShader GL init (`libc+0x8d70d` reads `NULL+0x308`). The window
tier is unstarted and blocked on pixels. The tobyOS kernel/net/crypto stack is
sound; the walls are inside chrome's binary (ML-KEM, SwiftShader) and in tobyOS's
GL support. Get the real error (NetLog / fault registers / objdump) before you
theorize, and clean-rebuild after any `struct proc` change.
