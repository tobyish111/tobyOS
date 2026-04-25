# Milestone 26 — Peripheral Support Expansion

**Status: COMPLETE** (all 7 phases + final validation suite pass)

M26 expands tobyOS peripheral support for practical real-world hardware
usability: USB hubs, hot-plug, HID stability, mass-storage validation,
HD Audio basics, and ACPI battery support.

The milestone is structured as seven sequential phases (M26A → M26G),
each with its own design / implementation / test / validation cycle,
plus a final aggregator (`test_m26_final.ps1`) that runs every phase
back-to-back and confirms no regressions in the shell, GUI, filesystem,
or networking subsystems.

---

## Implemented features

### M26A — Peripheral Test Harness

Reusable validation framework that all subsequent phases plug into.

- Kernel-side `devtest` module (`src/devtest.c`) with selftest registry
  (`devtest_register("xhci", ...)`) and bus-keyed enumeration
  (`devtest_enumerate(ABI_DEVT_BUS_*)`) returning unified
  `struct abi_dev_info` records.
- Userland tools:
  - `/bin/devlist` — enumerates every device on every bus.
  - `/bin/drvtest` — runs registered driver selftests.
  - `/bin/usbtest`, `/bin/audiotest`, `/bin/batterytest` —
    subsystem-specific clients of the same harness.
- Boot harness (`m26a_run_userland_tools` in `src/kernel.c`) spawns
  every tool sequentially via the shell so a regression in
  exec/spawn/PATH is also surfaced.
- Structured PASS / SKIP / FAIL output with diagnostic strings
  in every record's `extra` column.

### M26B — USB Hub Support

- USB hub class driver (`src/usb_hub.c`) parses `bDescriptorType=0x29`
  hub descriptors, walks downstream ports, and registers each
  enumerated child as its own `abi_dev_info` record on
  `ABI_DEVT_BUS_USB` with route-string visibility.
- xHCI controller (`src/xhci.c`) routes Address Device commands
  through the hub's parent slot for downstream-port children.
- One level of nesting validated; deeper trees should work but are not
  in the QEMU configurations the test scripts boot.

### M26C — USB Hot-Plug

- xHCI port-status-change events drive a kernel hot-plug poller
  (`xhci_service_port_changes` + `usb_hub_poll`) called from the idle
  loop; latency ~10 ms.
- Per-driver `*_unbind` hooks (`usb_hid_unbind`, `usb_msc_unbind`,
  `usb_hub_unbind`) tear down state cleanly when a slot disappears.
- Userland `usbtest hotplug` drains a kernel `abi_hot_event` ring
  (`tobydev_hot_drain`) and prints `+attach` / `-detach` lines.
- Repeated attach/detach cycles validated via QEMU QMP
  `device_add` / `device_del` — no slot leaks across 5 cycles.

### M26D — USB HID Robustness

- Multi-device support: simultaneous USB keyboards + mice register
  independently with per-device counters
  (`devs`, `kbd`, `mouse`, `frames`).
- Modifier keys (Shift, Ctrl, Alt, Meta) tracked and surfaced in the
  `usbtest hid` snapshot.
- Reconnect path: detach + reattach within the same boot leaves the
  USB HID slot count back at the original value with no leaked refs.
- PS/2 fallback preserved: when both USB and PS/2 keyboards are
  present, both increment their counters and feed the shell cleanly.

### M26E — USB Mass Storage Robustness

- `blk_dev` gone-flag with `blk_mark_gone()` makes future I/O return
  `-EIO` deterministically once the underlying device is yanked.
- Generic `vfs_unmount(path)` + per-FS `umount` op (`fat32_umount`)
  release mount references cleanly.
- USB MSC unbind hook warns if any active mount references the
  vanishing block device, then forces a `vfs_unmount` rather than
  crashing.
- Boot-time smoke test mounts `/usb`, writes 30 bytes, unmounts, and
  re-mounts — full round trip recorded in serial log.
- Yank-while-mounted scenario validated via QMP `device_del`:
  `[WARN]` printed, mount forced down, no panic.

### M26F — HD Audio Basic Output

- PCI detection of Intel HD Audio controllers (and QEMU's `intel-hda`
  emulation), MMIO BAR mapping, full CRST + ICH wake-up sequence.
- DMA-backed CORB / RIRB rings with verb send/receive plumbing,
  including per-codec timeout counters.
- Codec enumeration walks every Function Group and widget node,
  classifying DAC / ADC / PIN / MIX / SEL nodes.
- Best-effort tone playback: builds a Stream Descriptor + BDL,
  programs SD_FMT for 48 kHz stereo 16-bit PCM, writes a sine wave
  into a DMA buffer, and observes LPIB advance to confirm the
  controller is actually clocking the stream out.
- `audiotest` userland: lists controller + per-codec records, runs
  both `audio` and `audio_tone` selftests with full diagnostic output.
- Tone path is verb / DMA / LPIB-validated only — actual audible
  output is suppressed by QEMU's `none` audiodev backend, which is
  the right choice for hands-off CI.

### M26G — ACPI Battery Support

- `src/fw_cfg.c` minimal QEMU firmware-config PIO reader (signature
  probe at selector 0, file-directory walk at selector 0x0019,
  per-file content reads).
- `src/acpi.c` extended to harvest DSDT + every SSDT into
  `g_info.dsdt` / `g_info.ssdts[]`, with a generic
  `acpi_aml_find_bytes()` byte-search helper.
- `src/acpi_bat.c` rewritten with two cooperating discovery paths:
  - **fw_cfg mock** (`opt/tobyos/battery_mock`) — key=value blob
    injected from the QEMU command line, used by automated tests
    to force "battery present, charging, 75%" on machines with no
    native ACPI battery emulation.
  - **Heuristic AML scan** for the literal `"PNP0C0A"` _HID — a hit
    means the firmware DECLARED a battery; reading actual charge
    requires a real AML interpreter (deferred to M27+).
- `batterytest` userland prints the full battery record table and
  hints at the `-fw_cfg` syntax when no battery is detected.
- Graceful absence: on QEMU x86 with no mock provided, the kernel
  reports "no ACPI battery detected" and the userland tool reports
  `SKIP`, exit 0.

---

## Tests created

| Phase | Script | What it validates |
|-------|--------|-------------------|
| M26A | `test_m26a.ps1` | Boot harness inventory + every userland tool reports PASS |
| M26B | `test_m26b.ps1` | `usb-hub` device enumerated + downstream port walk in `devlist` |
| M26C | `test_m26c.ps1` | QMP `device_add` / `device_del` cycles produce ATTACH / DETACH events |
| M26D | `test_m26d.ps1` | QMP `input-send-event` keys / mouse / modifiers + reconnect cycle |
| M26E | `test_m26e.ps1` | mount + RW + unmount + QMP yank-while-mounted produces WARN + EIO |
| M26F | `test_m26f.ps1` | HDA controller probe, CORB/RIRB, codec walk, tone selftest PASS |
| M26G | `test_m26g.ps1` | Two scenarios: absent → SKIP cleanly; fw_cfg mock → PASS w/ all fields |
| ALL  | `test_m26_final.ps1` | Runs every phase + global regression sweep against M26A serial log |

---

## PASS/FAIL results (final aggregator run)

```
================== M26 PER-PHASE RESULTS ==================
Phase Verdict Elapsed Desc
----- ------- ------- ----
M26A  PASS        8.2 Peripheral test harness + userland tools
M26B  PASS        8.2 USB hub support (descriptors, port walk, devlist)
M26C  PASS       20.8 USB hot-plug (attach/detach, hot_event ring)
M26D  PASS       28.8 USB HID robustness (multi-dev, modifiers, reconnect, PS/2 fallback)
M26E  PASS       14.5 USB mass storage robustness (mount/unmount, safe removal)
M26F  PASS        9.2 HD Audio basic output (CRST, CORB/RIRB, codec walk, tone)
M26G  PASS       18.2 ACPI battery (heuristic AML scan + fw_cfg mock)

================== GLOBAL REGRESSION SWEEP ==================
Subsystem    Verdict
---------    -------
OS boot      PASS
Filesystem   PASS
Shell        PASS
GUI          PASS
Input (PS/2) PASS
Input (USB)  PASS
Network      PASS
ACPI         PASS
fw_cfg       PASS

================== M26 FINAL: PASS ==================
```

Total wall-clock for the full sweep: ~110 s (7 QEMU boots + 1 grep pass).

---

## Known limitations

- **M26B:** hub nesting depth 1 is what gets exercised in CI. Deeper
  trees should work because the route-string handling is generic, but
  the test scripts don't boot a `hub-of-hubs` topology.
- **M26C:** hot-plug uses idle-loop polling (~10 ms latency) instead
  of binding the xHCI port-status-change MSI vector. Simpler, but
  pegs the idle CPU until the first hot-plug event drains.
- **M26D:** input routing prefers the most-recently-attached HID;
  there is no per-window or per-grab arbitration yet.
- **M26E:** removal-while-mounted leaves the mount point in EIO-only
  mode until an explicit unmount. There's no auto-cleanup of stale
  mount entries beyond the forced `vfs_unmount` at unbind time.
- **M26F:** tone playback is verb/DMA-validated only — audible output
  not asserted (QEMU `none` backend on purpose, for hands-off CI).
  Codec mixer/jack-detect not exercised; only the first DAC + first
  PIN are walked.
- **M26G:** no AML interpreter — a `PNP0C0A` heuristic hit reports
  "present, charge unknown". Real `_BIF`/`_BST` evaluation is M27+
  work. Single battery only; multi-battery laptops will report only
  the first declared device. Heuristic byte scan won't match
  `_HID = EISAID("PNP0C0A")` packed-encoded form (rare in practice).

---

## QEMU validation commands

Per-phase scripts (each is independent and idempotent):

```powershell
pwsh .\test_m26a.ps1     # peripheral harness
pwsh .\test_m26b.ps1     # USB hub
pwsh .\test_m26c.ps1     # USB hot-plug
pwsh .\test_m26d.ps1     # USB HID
pwsh .\test_m26e.ps1     # USB mass storage
pwsh .\test_m26f.ps1     # HD Audio
pwsh .\test_m26g.ps1     # ACPI battery
```

Final aggregator:

```powershell
pwsh .\test_m26_final.ps1            # all phases + global sweep
pwsh .\test_m26_final.ps1 -Build     # rebuild kernel ISO first
pwsh .\test_m26_final.ps1 -Skip M26B # skip a phase by tag
```

Notable QEMU snippets used by the per-phase scripts:

```text
USB hub:
    -device qemu-xhci,id=usb0
    -device usb-hub,bus=usb0.0,id=hub1
    -device usb-kbd,bus=hub1.0
    -device usb-mouse,bus=hub1.0

Hot-plug (M26C):
    -qmp tcp:127.0.0.1:4444,server=on,wait=off
    # then use qmp shell for device_add/device_del

USB mass storage (M26E):
    -drive  if=none,id=usbstick,file=stick.img,format=raw
    -device usb-storage,bus=usb0.0,id=stick0,drive=usbstick

HD Audio (M26F):
    -audiodev none,id=hda
    -device   intel-hda,id=hda0
    -device   hda-duplex,audiodev=hda

ACPI battery mock (M26G):
    -fw_cfg name=opt/tobyos/battery_mock,string=state=charging;percent=75;design=50000;remaining=37500;rate=1500
```

---

## Real-hardware test checklist

```
[ ] Boot the ISO on a bare-metal x86_64 PC with USB ports + a USB hub.
[ ] Run 'devlist'. Confirm at least one xhci/uhci/ehci PCI bus
    + 1 hub + 1 HID + 1 BLK.
[ ] Run 'usbtest hub'. Hub descriptor + downstream port count must
    match the physical hub.
[ ] Run 'usbtest hotplug'. Unplug + replug a stick; expect ATTACH then
    DETACH events in the drain output.
[ ] Plug in a USB keyboard + mouse together. Type into the shell,
    click in the GUI -- both must work.
[ ] Plug in a FAT32-formatted USB stick. Run 'usbtest storage'.
    mount / RW / unmount must all PASS.
[ ] Yank the USB stick WHILE mounted -- expect a [WARN] line and EIO
    from subsequent reads, not a panic.
[ ] On a laptop with HD Audio: run 'audiotest'. Controller PASS + at
    least 1 codec record expected.
[ ] On a laptop with a battery: run 'batterytest'. Expect
    'PNP0C0A detected in DSDT...' + selftest PASS.
[ ] On a desktop without a battery: run 'batterytest'. Expect SKIP
    path, no FAIL.
[ ] Unplug the USB hub mid-session. xhci_service_port_changes should
    clean up downstream slots without leaks.
[ ] PS/2 fallback: boot on a machine with a PS/2 keyboard. Confirm
    '[ps2_kbd]' chars increment as you type.
```

---

## File inventory

### New kernel source

- `include/tobyos/fw_cfg.h`, `src/fw_cfg.c` — QEMU fw_cfg PIO reader
- `include/tobyos/usb_hub.h` (extended), `src/usb_hub.c` — USB hub
  class driver (M26B/C)
- `include/tobyos/devtest.h`, `src/devtest.c` — peripheral test
  harness (M26A)
- `include/tobyos/hotplug.h`, `src/hotplug.c` — hot-plug event ring
  (M26C)
- `include/tobyos/audio_hda.h` (rewritten), `src/audio_hda.c`
  (rewritten) — HDA driver with codec walk + tone selftest (M26F)

### Modified kernel source

- `include/tobyos/acpi.h`, `src/acpi.c` — DSDT/SSDT inventory +
  byte-search helper
- `include/tobyos/acpi_bat.h`, `src/acpi_bat.c` — heuristic AML scan
  + fw_cfg mock + introspect / selftest
- `include/tobyos/blk.h`, `src/blk.c` — `blk_mark_gone` + EIO-only
  fallthrough
- `include/tobyos/vfs.h`, `src/vfs.c` — `vfs_unmount` +
  `vfs_iter_mounts`
- `src/fat32.c` — `fat32_umount` op
- `src/usb_hid.c`, `src/usb_msc.c` — per-device counters,
  introspect/selftest, unbind hooks
- `src/xhci.c` — port-status-change service loop, `xhci_detach_slot`,
  hub route handling
- `src/kernel.c` — boot harness orchestration, fw_cfg + battery init

### Userland

- `programs/devlist`, `programs/drvtest`, `programs/usbtest`,
  `programs/audiotest`, `programs/batterytest` — all M26x clients
- `libtoby/include/tobyos_devtest.h`, `libtoby/src/devtest.c` —
  shared introspection helpers (`tobydev_list`, `tobydev_test`,
  `tobydev_print_record`, `tobydev_hot_drain`)

### Validation scripts

- `test_m26a.ps1` ... `test_m26g.ps1` — per-phase
- `test_m26_final.ps1` — aggregator

---

## What's next

Logical M27 candidates that build on M26:

- **AML interpreter**: minimal `_BIF` / `_BST` evaluator so M26G
  reports real charge percentage on laptops.
- **xHCI MSI hot-plug interrupt**: replace the idle-loop poll with
  a true interrupt-driven port-status-change handler.
- **Audio routing**: mixer / volume / jack-detect, multiple streams,
  ALSA-style userspace API.
- **Multi-battery + thermal zones** via the same heuristic-AML
  pattern (PNP0C0A → multiple BAT0/BAT1, `_TZ_` thermal zone walk).
