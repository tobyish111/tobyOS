# Milestone 35 — Hardware Compatibility & Robustness

**Status: COMPLETE** (all seven sub-milestones plus the integrated
`compattest` tool pass under QEMU)

M35 widens the set of hardware tobyOS recognises, makes every
unsupported case fail loudly-but-safely, and gives the operator
real visibility into *what bound, what didn't, and why*. Nothing in
this milestone touches host hardware, modifies firmware, or runs
privileged host operations: every automated test runs inside QEMU,
and the only "real hardware" deliverable is a manual checklist
emitted by `/bin/compattest --checklist`.

The milestone is structured as seven phases (M35A → M35G), each
with its own design / implementation / test / validation cycle. Two
PowerShell drivers (`test_m35.ps1` and `test_m35g.ps1`) re-exercise
the entire surface from a clean kernel image and exit non-zero on
the slightest regression.

---

## Implemented compatibility improvements

### M35A — Expanded driver matching database

A new static driver knowledge base (`drvdb`) catalogues every PCI
and USB device tobyOS either drives, would like to drive next, or
has consciously decided to skip. Each entry carries:

- bus + selector (PCI vendor/device or USB class/sub/proto)
- canonical driver name (or `unknown`)
- support tier — `supported`, `partial`, `unsupported`
- short human-readable reason

Sitting on top of `drvdb` is `drvconf`, which parses
`/etc/drvconf` (lines of `force vendor:device driver` and
`blacklist driver` / `blacklist vendor:device`) at boot. The PCI
driver-binding pass is now a deterministic two-pass walk:

1. **Pass 1** records every PCI function into `drvmatch` with the
   strategy that *would* be used (`exact`, `class`, `generic`,
   `forced`, `blacklisted`, `none`).
2. **Pass 2** binds the chosen driver, honouring force/blacklist
   overrides from `drvconf`, and records the outcome
   (`drvmatch_status_*`).

Failure mode is fail-soft and *visible*: a blacklisted device
appears in `drvmatch` with `strategy=blacklisted status=skipped`,
and a forced-but-not-installed driver appears with
`strategy=forced status=missing`. Either way the kernel never
panics and the operator can see the decision in `/bin/drvmatch`.

A new `drvmatch_count(total, bound, unbound, forced)` helper lets
the selftest harness assert exact bind counts without re-walking
the table.

### M35B — Additional virtual hardware support

A modern non-transitional **virtio-blk-pci** driver
(`src/virtio_blk.c`) was implemented from scratch:

- Matches Red Hat vendor `0x1AF4` at devices `0x1001` (transitional)
  and `0x1042` (modern). Either way it drives the V1 transport
  and declines cleanly if a transitional device doesn't expose the
  modern caps.
- Single virtqueue, polled completion, one in-flight request at a
  time. Synchronous `read_sectors` / `write_sectors` against any
  contiguous LBA range.
- Registers each detected device with the existing `blk` subsystem
  as `vblk0`, `vblk1`, …, so partition scan + `/data` mount logic
  work unchanged.
- Bounded by the milestone-21 PCI driver registry, so it falls
  under the same `drvdb` / `drvmatch` / `drvconf` pipeline as
  every other driver.

`virtio-net` (M22) and `virtio-input` (M23 hooks) were already in
tree; M35B confirmed both still bind cleanly in the new
`drvmatch`-driven world via the M35B selftest.

### M35C — USB device class expansion

The USB stack now records *every* attached device into a per-bus
attach registry (`usbreg`) — bound or not — so `hwdb` can
distinguish "we drive this" from "we saw it but no driver matched".

- `usbreg_record_attach(dev, drv_name, bound)` — every successful
  finalize emits one line.
- `usbreg_record_probe_failed(dev, drv_name, err)` — drivers that
  match the class but fail to bind (e.g. unsupported subclass) are
  recorded explicitly.
- `usbreg_clear_for_port(port)` — disconnect path cleans up so
  hot-plug is stable.

`drvdb`'s USB catalogue grew to cover:

- HID (kbd, mouse, joystick, "boot mode" + "report mode" subclasses)
- mass storage (SCSI transparent + UFI + RBC subclasses)
- hub
- vendor-specific class — recognised as `unsupported` with a clean
  reason rather than a probe attempt

Unsupported USB devices now appear in `/bin/hwcompat` with
`status=unsupported reason="USB class N: not implemented"` instead
of any kernel-side write. xHCI continues to enumerate them so the
audit trail is complete.

### M35D — Hardware compatibility database

A new in-OS database (`src/hwdb.c`) snapshots compatibility status
across PCI + USB:

- Each row is an `abi_hwcompat_entry` (frozen ABI in
  `include/tobyos/abi.h`):
  - `bus` (PCI / USB) + `selector` (vendor:device or class triple)
  - `driver` name (or empty)
  - `status` — `supported` / `partial` / `unsupported` / `unknown`
  - `reason` short string, max length stable for tests to grep
- `hwdb_snapshot(rows, max)` returns the live state, computed at
  call time from PCI enumeration + `usbreg` + `drvdb` + the live
  `drvmatch` outcome. No background tasks; no caches to invalidate.
- `hwdb_counts(supp, part, unsupp, unknown)` for cheap tallies.

A new syscall `ABI_SYS_HWCOMPAT_LIST` exposes the snapshot to
userland. The new `/bin/hwcompat` tool renders it as a table:

```
PCI 1AF4:1042 virtio-blk     supported    "modern virtio-blk"
PCI 1AF4:1041 virtio-net     supported    "modern virtio-net"
PCI 8086:100E e1000          partial      "tx-only, rx polled"
USB 03:01:01 usb_hid         supported    "boot keyboard"
USB 09:00:00 (none)          unsupported  "USB hub: not driven"
...
```

### M35E — Boot profiles & driver modes

The pre-existing `safemode` infrastructure (M28D / M29C) was
extended with a fourth level and a richer skip-policy API:

| Level                          | Tag             | ABI boot mode |
|--------------------------------|-----------------|---------------|
| `SAFEMODE_LEVEL_NONE`          | `normal`        | `0`           |
| `SAFEMODE_LEVEL_BASIC`         | `safe-basic`    | `1`           |
| `SAFEMODE_LEVEL_GUI`           | `safe-gui`      | `2`           |
| `SAFEMODE_LEVEL_COMPATIBILITY` | `compatibility` | `4`           |

(ABI mode `3` is reserved for `verbose`.)

`COMPATIBILITY` boots like `default` but skips the driver groups
that empirically cause trouble on quirky hardware:

- audio
- USB classes beyond HID (no hub, no mass storage)
- virtio-gpu fast path (falls back to plain framebuffer)

The skip predicates are now precise — `safemode_skip_audio()`,
`safemode_skip_gui()`, `safemode_skip_services()`,
`safemode_skip_virtio_gpu()`, `safemode_skip_usb_full()`,
`safemode_skip_usb_extra()` — so each subsystem can opt in to the
narrowest possible gate. `kernel.c` and `xhci.c` were rewritten to
use these instead of the blunt `safemode_active()` boolean.

Selection at boot is unchanged: `/etc/safemode_level` (preferred,
built with `make SAFEMODE_LEVEL=...`), the legacy
`/etc/safemode_now` flag, or `safemode_force(level)` at runtime.

### M35F — Enhanced diagnostics & reporting

`hwinfo` now records the precise `ABI_BOOT_MODE_*` value (not just
"safe / not safe"), and `tobyhw_boot_mode_str` renders it as a
human-readable name (`normal`, `safe-basic`, `safe-gui`,
`verbose`, `compatibility`).

A new userland tool `/bin/hwreport` wraps `hwinfo` + `hwcompat` +
`drvmatch` and emits a single, operator-readable verdict:

| Verdict   | Meaning                                                     |
|-----------|-------------------------------------------------------------|
| `GREEN`   | Every detected device is supported and bound; no warnings.  |
| `YELLOW`  | Some devices partial / unsupported, but every required class is covered. |
| `RED`     | A required class (any of: PCI bus has zero bound devices, or any device in `unknown` state) is missing. |

`hwreport` supports four output modes:

```
hwreport            # pretty multi-section human report
hwreport --summary  # one line: verdict + tallies
hwreport --json     # machine-readable for tooling
hwreport --boot     # M35F_HWR sentinels for test_m35*.ps1
```

A boot-time harness (`m35f_run_hwreport_harness` in `kernel.c`)
spawns `/bin/hwreport --boot` automatically so every boot logs the
sentinel verdict to the serial console:

```
M35F_HWR: boot_mode=normal profile=vm
M35F_HWR: total=11 pci=8 usb=3 supported=7 partial=3 unsupported=1 unknown=0
M35F_HWR: bound pci=4 usb=3 probe_failed=0 forced_off=0
M35F_HWR: verdict=YELLOW
M35F_HWR: PASS
```

### M35G — Final compatibility validation (VM-only)

`/bin/compattest` is the integrated end-to-end suite. It exercises
eight "buckets" against the live, production kernel:

| Bucket            | What it asserts                                                |
|-------------------|----------------------------------------------------------------|
| `SYSTEM_BOOT`     | `tobyhw_summary` populated; ABI version sane; CPU, memory, paging known; snapshot epoch advanced. |
| `DRIVER_MATCH`    | At least one PCI device bound to a real driver; `drvmatch` snapshot consistent with `hwdb` PCI rows. |
| `FALLBACK_PATHS`  | A bogus `tobyhw_drvmatch(0xDEAD:0xBEEF)` returns `ENOENT` cleanly; `class`/`generic` strategies appear in the live snapshot when applicable. |
| `NETWORK`         | At least one NIC bound (virtio-net / e1000 / e1000e). |
| `STORAGE`         | `tobydev_list` reports ≥1 block device; `hwinfo` agrees on the count; first block device has a non-empty name. |
| `USB_INPUT`       | At least one HID device bound when xHCI + usb-kbd/mouse are attached. SKIPPED (`SKIPPED_REAL_HARDWARE_REQUIRED`) on configs without USB. |
| `LOG_CAPTURE`     | `tobylog_stats.total_emitted > 0`; `tobylog_read` returns ≥1 record; ≥1 record has `level >= INFO`. |
| `NO_CRASHES`      | A `tobylog_write` round-trip survives; the bogus drvmatch returned ENOENT instead of crashing the kernel. |

Tests that require physical hardware (e.g. real Wi-Fi, real audio,
real BMC) are explicitly returned as
`SKIPPED_REAL_HARDWARE_REQUIRED` rather than failing — the test
script tolerates SKIP, fails on FAIL.

`compattest` supports four output modes:

```
compattest               # pretty human report, exit code = verdict
compattest --json        # machine-readable, ditto
compattest --boot        # M35G_CMP sentinels for test_m35g.ps1
compattest --checklist   # the manual real-hardware checklist (see below)
```

A boot-time harness (`m35g_run_compattest_harness` in `kernel.c`)
spawns `/bin/compattest --boot` on every boot of the production
kernel so the eight buckets are validated *every time the OS comes
up*, not just under selftest.

---

## Tests

Two PowerShell drivers, each shipping with the source tree:

| Driver           | What it runs                                                       | Build target                          |
|------------------|--------------------------------------------------------------------|---------------------------------------|
| `test_m35.ps1`   | M35A / B / C / D / E / F kernel-side selftests inside boot         | `make m35test` (`-DM35_SELFTEST`)     |
| `test_m35g.ps1`  | M35F+G boot-time harness + `/bin/compattest --boot` end-to-end     | `make all` (production kernel)        |

Both drivers boot tobyOS headless under QEMU with serial + debug
captured to disk, wait for a canonical marker line, and exit 0
only on PASS.

### Final results

```
$ pwsh -File test_m35.ps1
[m35] OVERALL: PASS
   m35a : PASS  (total=8 fail=0 skipped=0)
   m35b : PASS  (total=5 fail=0 skipped=0)
   m35c : PASS  (total=7 fail=0 skipped=0)
   m35d : PASS  (total=7 fail=0 skipped=0)
   m35e : PASS  (total=9 fail=0 skipped=0)
   m35f : PASS  (total=7 fail=0 skipped=0)

$ pwsh -File test_m35g.ps1
[m35g] OVERALL: PASS
   M35G_CMP: SYSTEM_BOOT     : PASS
   M35G_CMP: DRIVER_MATCH    : PASS
   M35G_CMP: FALLBACK_PATHS  : PASS
   M35G_CMP: NETWORK         : PASS
   M35G_CMP: STORAGE         : PASS
   M35G_CMP: USB_INPUT       : PASS
   M35G_CMP: LOG_CAPTURE     : PASS
   M35G_CMP: NO_CRASHES      : PASS
   M35G_CMP: VERDICT         : PASS pass=8 fail=0 skipped=0
```

**Combined: 51 / 51 PASS, 0 FAIL, 0 SKIP.**

(`test_m35.ps1` covers 43 selftest steps across the six
`-DM35_SELFTEST` phases; `test_m35g.ps1` covers the eight
`compattest` buckets on the live kernel. There is overlap by
design — the selftests assert kernel-internal invariants while
`compattest` asserts userland-observable invariants.)

---

## QEMU validation commands

The most useful invocations during M35 development:

```bash
# Production build, no test gating. Verifies the production kernel
# still boots cleanly and the hwreport / compattest boot harnesses
# both fire and emit verdict=GREEN|YELLOW.
make all
make run                                      # interactive QEMU

# At the prompt:
#   tobyOS> hwinfo
#   tobyOS> hwcompat
#   tobyOS> hwreport
#   tobyOS> compattest               # full report
#   tobyOS> compattest --checklist   # manual hardware checklist

# M35A / B / C / D / E / F kernel-side selftest harness.
make m35test                                  # rebuilds with -DM35_SELFTEST
pwsh -File test_m35.ps1                       # default: all six phases

# M35G live compattest validation against the production kernel
# (no rebuild needed if `make all` is current).
pwsh -File test_m35g.ps1

# Reproduce a specific QEMU configuration the suite uses:
qemu-system-x86_64 \
    -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -smp 4 -serial file:serial.log -debugcon file:debug.log \
    -no-reboot -no-shutdown -display none \
    -nic user,model=virtio-net-pci \
    -drive file=build/vblk_test.img,format=raw,if=none,id=vblk0 \
    -device virtio-blk-pci,drive=vblk0,disable-legacy=on,disable-modern=off \
    -device qemu-xhci,id=xhci -device usb-kbd -device usb-mouse \
    -drive file=build/usb_test.img,format=raw,if=none,id=ums0 \
    -device usb-storage,drive=ums0
```

The PowerShell drivers also accept `-WithVirtioBlk:$false` and
`-WithUsbStack:$false` to reproduce a stripped-down config (the
selftests still PASS — the affected steps return `SKIPPED` rather
than `FAIL`).

---

## Known limitations

These are intentional scope-management decisions, not bugs. Each
one is documented inline next to the relevant code.

1. **No real-hardware test automation.** Per the M35 brief, tobyOS
   never probes host hardware from inside the kernel and never
   issues privileged host operations. Real-hardware validation is
   operator-driven via `/bin/compattest --checklist` (reproduced
   below) and is **not** wired into CI.
2. **virtio-blk is single-queue, polled.** A future milestone can
   add MSI-X interrupts and pipelined requests; the current
   driver intentionally keeps the surface tiny so M35B's selftest
   has a fully-deterministic happy path.
3. **`drvconf` is text-only.** `/etc/drvconf` is a line-oriented
   file. Live re-loading and a `drvconf` shell command are out of
   scope for M35; today the pipeline is "edit, reboot, verify with
   `/bin/drvmatch`".
4. **`hwreport`'s `RED` verdict is conservative.** Any device in
   `unknown` state, or zero PCI binds, drops the verdict to RED.
   This errs on the side of warning the operator and is documented
   on the verdict line itself.
5. **USB hub support is not implemented.** Hubs appear in
   `hwcompat` as `unsupported reason="USB hub: not driven"`. A
   single-tier (root-port-only) topology is sufficient for QEMU
   `qemu-xhci`; nested hubs are tracked for a future milestone.
6. **Compatibility profile is not a microkernel.** It still loads
   networking + GUI + services; it only skips the *flaky* driver
   groups (audio, non-HID USB, virtio-gpu fast path). For a true
   minimal boot use `safe-basic` (kernel + shell only).

---

## Manual real-hardware checklist

The same text is also produced by `/bin/compattest --checklist`
on a live tobyOS install. Reproduced here for the milestone
record:

```
# tobyOS M35 manual hardware checklist

Run AFTER every fresh tobyOS install on real hardware. Each item
is operator-driven; tobyOS does not (and must not) probe these
from inside the kernel.

## boot
- [ ] machine POSTs and Limine menu is visible
- [ ] `default` profile boots to the desktop without errors
- [ ] `safe-basic` profile boots to /bin/safesh prompt
- [ ] `safe-gui`   profile boots to a minimal GUI session
- [ ] `compatibility` profile boots with reduced drivers
      (no audio, no non-HID USB, no virtio-gpu fast path)
- [ ] no kernel panics, late hangs, or boot loops in any profile

## input
- [ ] PS/2 keyboard registers keystrokes in the shell + GUI
- [ ] PS/2 mouse moves the cursor and reports button events
- [ ] USB keyboard plug-in is recognised and types correctly
- [ ] USB mouse plug-in moves the cursor and reports buttons
- [ ] hot-unplug of USB input device produces a clean detach log
      (no driver crash, slog records the event, devlist updates)

## storage
- [ ] tobyOS detects the install disk under /data
- [ ] reading and writing files under /data succeeds
- [ ] USB mass-storage stick mounts under /usb when plugged in
- [ ] removing the USB stick produces a clean detach log
- [ ] virtio-blk (if present) registers as vblk0 and round-trips
      a sentinel sector via blktest

## network
- [ ] NIC link comes up (ip link via /bin/netecho -- IPv4 lease)
- [ ] DHCP succeeds and DNS resolution works
- [ ] /bin/netecho can complete an ICMP/UDP round trip to gateway
- [ ] disconnecting the cable produces a NIC down log
      and reconnect re-leases an IP

## display
- [ ] framebuffer is set up by the time login screen appears
- [ ] desktop renders icons + cursor at the native resolution
- [ ] gui_about, gui_settings, and gui_term all open and paint
- [ ] virtio-gpu fast path (if available) is engaged in `default`
      and skipped in `compatibility` mode

## logs
- [ ] /bin/logview shows boot lines, driver attach lines, and
      services lines without truncation
- [ ] /data/last_boot.diag exists and is current
- [ ] /data/hwinfo.snap exists and matches /bin/hwinfo output
- [ ] /bin/hwcompat shows every detected device with a status
- [ ] /bin/hwreport renders GREEN or YELLOW verdict (RED is bad)

## audit
- [ ] reboot the system; counts in /bin/hwreport survive across
      reboots (snapshot_epoch advances; boot_seq increments)
- [ ] /bin/compattest reports VERDICT: PASS on the live system
      (skipped buckets are acceptable, fails are not)
```

---

## Files added or substantially changed

```
NEW   include/tobyos/drvdb.h          static driver knowledge base
NEW   include/tobyos/drvconf.h        force/blacklist parser API
NEW   include/tobyos/usbreg.h         USB attach registry API
NEW   include/tobyos/hwdb.h           in-OS compatibility DB API
NEW   src/drvdb.c                     PCI + USB catalogue + lookup
NEW   src/drvconf.c                   /etc/drvconf parser + applier
NEW   src/usbreg.c                    per-bus USB attach registry
NEW   src/hwdb.c                      compatibility snapshot logic
NEW   src/virtio_blk.c                modern virtio-blk-pci driver
NEW   src/m35selftest.c               M35A-F kernel selftests
NEW   programs/hwcompat/main.c        /bin/hwcompat user tool
NEW   programs/hwcompat/program.ld    linker script
NEW   programs/hwreport/main.c        /bin/hwreport user tool
NEW   programs/hwreport/program.ld    linker script
NEW   programs/compattest/main.c      /bin/compattest user tool
NEW   programs/compattest/program.ld  linker script
NEW   test_m35.ps1                    M35A-F kernel selftest driver
NEW   test_m35g.ps1                   M35G live-validation driver
NEW   M35_SUMMARY.md                  this document

CHG   include/tobyos/abi.h            abi_hwcompat_entry,
                                      ABI_SYS_HWCOMPAT_LIST,
                                      ABI_BOOT_MODE_COMPATIBILITY,
                                      ABI_DRVMATCH_* strategies
CHG   include/tobyos/safemode.h       SAFEMODE_LEVEL_COMPATIBILITY,
                                      safemode_skip_*() predicates
CHG   include/tobyos/drvmatch.h       drvmatch_count(),
                                      drvmatch_status_* enum
CHG   libtoby/include/tobyos_hwinfo.h boot_mode field, *_str helpers
CHG   libtoby/src/hwinfo.c            stores ABI boot-mode value;
                                      tobyhw_boot_mode_str()
CHG   src/safemode.c                  COMPATIBILITY level + skip
                                      predicates
CHG   src/drvmatch.c                  driver registry + counts;
                                      strategy/status tracking
CHG   src/pci.c                       two-pass bind; drvconf hook;
                                      hwdb wiring
CHG   src/xhci.c                      usbreg recording + safemode
                                      gating in finalize_device
CHG   src/usb_hid.c                   probe gating + class subsets
CHG   src/usb_msc.c                   safemode gating + skip path
CHG   src/usb_hub.c                   recognised-but-unsupported path
CHG   src/syscall.c                   ABI_SYS_HWCOMPAT_LIST dispatch
CHG   src/kernel.c                    drvconf_load+apply, hwdb_init,
                                      m35*_selftest (A-F),
                                      m35f_run_hwreport_harness,
                                      m35g_run_compattest_harness,
                                      safemode_skip_* gating
CHG   Makefile                        drvdb/drvconf/usbreg/hwdb/
                                      virtio_blk/m35selftest/safemode
                                      objects, hwcompat/hwreport/
                                      compattest programs, m35test
                                      target, initrd staging
```

---

## Final checklist

- [x] Expanded driver matching DB with PCI + USB IDs (M35A) — implemented + tested
- [x] Driver overrides (blacklist + force) (M35A) — implemented + tested
- [x] Modern virtio-blk-pci driver (M35B) — implemented + tested
- [x] virtio-net + virtio-input verified under new pipeline (M35B) — implemented + tested
- [x] USB device class expansion (M35C) — implemented + tested
- [x] Unsupported USB devices fail safe + visible (M35C) — implemented + tested
- [x] In-OS hardware compatibility database + `/bin/hwcompat` (M35D) — implemented + tested
- [x] Boot profiles incl. `compatibility` mode (M35E) — implemented + tested
- [x] Per-subsystem `safemode_skip_*` predicates (M35E) — implemented + tested
- [x] `/bin/hwreport` + boot harness with verdict (M35F) — implemented + tested
- [x] `/bin/compattest` + boot harness with eight-bucket suite (M35G) — implemented + tested
- [x] Manual real-hardware checklist via `/bin/compattest --checklist` (M35G) — implemented
- [x] Default build still passes a clean boot (no regressions vs. M34)
- [x] No host hardware probed; no firmware/boot settings touched
- [x] All automated tests run inside QEMU
- [x] All unsupported hardware fails gracefully with audit-visible reason
- [x] Documentation updated (this file)
