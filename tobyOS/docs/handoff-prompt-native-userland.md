# Handoff prompt — native Linux userland for tsh

Paste the block below to the next agent. Everything it references is in the tree.

---

You are continuing work on **tobyOS** (`c:\CustomOS`), a from-scratch OS whose shell
is `tsh`. Your job is to make the standard Linux system-inspection commands **native
tobyOS programs**, and to add the kernel data sources they need. The user's directive:
*"I would eventually like tsh to have all of these libraries natively."*

## Read these first, in this order

1. `~/.claude/projects/c--CustomOS/memory/MEMORY.md`, then at minimum these entries:
   - **`linux-userland-commands`** — the immediately preceding slice. It has the
     applet-linking design AND the traps that cost build cycles. Read it before you
     touch the initrd rule.
   - `shell-bash-parity-gate` and `shell-oilspec-third-party-gate` — the two shell
     gates you must not regress. **Read the parity file before adding shell cases.**
   - `tobyos-build-env`, `initrd-tar-explicit-list` — build laws.
   - `linux-alpine-slice7` — the existing "run real packages" path (apk in a chroot).
     It matters for scoping (see *What "all of them" honestly means*).
   - `real-hardware-elitedesk-bringup` — several of your targets read MSRs and SMBIOS,
     which behave differently on the EliteDesk than in QEMU.
2. `docs/handoff-prompt-phase3-close.md` — the previous handoff, for house style and
   the standing traps that still apply.

## Standing directives

- **tobyOS is ONE environment running BOTH Linux and Windows software.** Track C
  (Win32/PE) is co-equal. Never break a personality boundary.
- **Do not fabricate data.** This is the load-bearing rule for this arc. Every tool
  here reports facts about the machine, and a plausible-looking wrong number is worse
  than an honest "unavailable". The previous slice existed partly because
  `/proc/cpuinfo` had been answering `GenuineTobyOS` / `tobyOS virtual x86-64 CPU`
  for every core while the kernel had real CPUID strings cached the whole time. If a
  value cannot be obtained, omit the field or print `unknown` — never invent one.
  (`statfs` is on record in memory as fabricated; do not add to that list.)
- **Report honestly.** Partial is fine; overclaiming is not.

## Non-negotiable working rules

- **Gates.** Before you start and after every slice:
  - `bash logs/lxposix.sh` — must stay **GREEN 32/32, skipped=0, enosys_gaps=0**.
  - `bash logs/shparity.sh` — **98/98**. Anything that adds a name to `/bin` can
    change what a `command -v` or "not found" case sees.
  - `bash logs/oilspec.sh` — compare against the baseline diff, not the raw verdict
    line (it always counts bash-only slots; see the memory file).
  - `bash logs/build_usb.sh && bash logs/isoboot.sh` — the shipped desktop must boot
    with **0 faults** before you call anything done.
  - `logs/lxsock.sh` needs its **own** `-DLXSOCK_BOOT` build; it will report
    "SERVER NEVER LISTENED" against any other flavour.
- **`logs/build_usb.sh` and `logs/cwnet.sh` DELETE `tobyOS.iso`.** If the user is
  holding an image to flash, copy it aside first.
- **Build environment.** `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`,
  and carry a Windows-native TMP on the compiler vars:
  `make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" ...`
- **`EXTRA_CFLAGS` reaches KERNEL objects only.** User programs need
  `PROG_EXTRA_CFLAGS`.
- **`EXTRA_CFLAGS` changes rebuild NOTHING.** `touch src/kernel.c`, or `rm -f src/*.o`
  on a flavour switch. On struct growth a full `make clean` is still the cheap safe
  habit — but **the "there is no header dependency tracking" clause this line used to
  carry is STALE** (2026-08-24): every object compiles with `-MMD -MP` and
  `Makefile:5433` does `-include $(ALL_DEPS)`, so editing a header does rebuild its
  dependents. `$(wildcard)` only picks up `.d` files that already exist, so a
  brand-new object is unprotected on its first build.
- **Never hide a build behind a grep.** A failed build leaves the PREVIOUS iso in
  place and you will test stale bits. Gate on the binary: assert the new marker is
  present **and the old one is gone**.
- **A new `/bin` program needs FOUR Makefile edits**, and missing any one fails
  differently: the `LIBTOBY_PROGRAM_RULES` eval, the `$(PROGRAMS)` list, the
  `cp ... $(INITRD_STAGE)/bin/<name>` staging line, and the **explicit tar member
  list**. It also needs a `program.ld` in its directory (copy `programs/devlist/`'s).
- **Edit the Makefile with the Edit tool, not scripted Python.** Two scripted edits in
  the last slice inserted literal `\n` two-character sequences and broke the build with
  `No rule to make target '\n'`.
- **CRLF WILL SILENTLY DEFEAT YOU.** MSYS resolves `bin/ls\r` as if the CR were not
  there, so `[ -e "…/$name" ]` answers **true** for names that do not exist. In the
  last slice that made a staging loop skip all 400 entries as "already taken" and
  report success having linked nothing. Any new list file read by the build needs a
  `.gitattributes` `text eol=lf` entry **and** a defensive CR strip.
- **Harnesses lie.** Assert the **value**, not that something exited 0. The best
  findings in this codebase are all green tests with a wrong number.
- **Measurement hygiene.** `taskkill //F //IM qemu-system-x86_64.exe` before believing
  any run; never reuse a log filename across runs; check log timestamps before reading
  a result.

## How to verify a command actually works in the guest

Do not reason from the host about whether a guest command runs. Use the harness that
already exists for exactly this — `-DPKGPROBE_BOOT` in `src/kernel.c` spawns
`/bin/busybox sh -c '<cmd>'` for a table of probes and prints each one's output and
exit status to serial. Add your probes to that table, build with
`EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DPKGPROBE_BOOT"`, and read the
`[PKGPROBE]` lines. That harness is how the previous slice found that `lspci` was
statting every sysfs file successfully and *still* printing `(null)`.

Note `/bin/head` is the native one and accepts only `-n N`, not `-20`. Several probe
failures in the last slice were the probe's own syntax, not the system's.

## Tree state

Branch `feat/posix-shell`, pushed, remote `github.com/tobyish111/tobyOS.git`.
Recent relevant commits: `0c0e1c1` (userland commands + sysfs PCI + native
lspci/lscpu), `cf2de49` (mouse wheel), `631973d` (IPv6 wire).

Gate state at handoff: lxposix **32/32** skipped=0 enosys_gaps=0 faults=0; lxsock
15/15; shipped ISO boots to desktop, 23 heartbeats, 0 faults.

**Already done — do not redo:**
- ~360 busybox applets hard-linked onto PATH (`programs/busybox/applets.txt`).
- `/sys/bus/pci/devices/<addr>/` fully populated, including `uevent`.
- Native `lspci` and `lscpu` (`programs/lspci`, `programs/lscpu`) — **these are your
  templates.** Both are deliberately `/proc` + `/sys` consumers, so a correct result
  also proves the tree is usable by ordinary Linux software.
- `/proc/cpuinfo` now reports real CPUID vendor/brand/family/model/stepping.
- `struct sysfs_node` gained a `fixed` field (pre-rendered content) because sysfs
  generators take no context and one function cannot serve N devices.
- **SLICE 1 IS DONE (`e98e3de`).** `/sys/bus/usb/devices` + native `lsusb`, with
  real `GET_DESCRIPTOR(STRING)` manufacturer/product/serial. Gate:
  `bash logs/usbsysfs.sh` (11/11). **It exists because every other gate boots QEMU
  with NO USB controller at all** — lxposix/shparity/oilspec would all stay green
  with the whole USB tree broken. Add a similar attach-the-hardware gate for any
  slice below whose data source the standard harnesses do not exercise.
  Known limit, stated in the code: the tree is a boot SNAPSHOT, so a hot-plugged
  device reaches `usbreg` but not `/sys`.

**Two things this handoff previously told you that turned out to be wrong** — the
build-law fix above, and:
- **`bash logs/oilspec.sh` has no committed baseline.** `oilspec_report.py` diffs
  against `logs/oilspec_prev.json`, which is *the last run's* archive, and
  `logs/oilspec_failures.txt` is gitignored. A stale archive framed slice 1 as a
  25-case regression (11 POSIX) when its failure set was byte-identical to HEAD's.
  To clear yourself: copy `oilspec_failures.txt` aside, `git stash`, `make clean`,
  re-run, and compare the two failure SETS — two empty symmetric differences means
  zero regressions. Cases 2272/2273 flip on their own and are noise.
  **The true baseline as of 2026-08-24 is POSIX 1271/1280 = 99.30%, not the 100%
  on record**; the nine failures are named in the `shell-oilspec-third-party-gate`
  memory. They are pre-existing and were not chased.

## What "all of them" honestly means — read before scoping

"All the standard Linux packages" is coreutils + util-linux + procps + pciutils +
usbutils + lm-sensors + kmod + … — a distro, not a slice, and reimplementing it
natively is neither achievable nor desirable. Two things are already true and should
shape what you promise the user:

- ~360 commands already resolve on PATH via busybox. Rewriting a working `seq` or
  `hexdump` natively buys nothing.
- For genuinely arbitrary packages the answer already exists: **real Alpine + `apk`
  in a chroot** (memory `linux-alpine-slice7`).

So the useful reading of "natively" is: **the machine-inspection tools that busybox
either lacks or gets wrong, and which need a kernel data source we do not yet
publish.** Those are the slices below. If you find yourself writing a native `cat`,
stop and re-read this section.

## Your slices, in priority order

Each slice is: **kernel data source → sysfs/procfs surface → native tool → probe →
gates → commit**. Do not skip the surface and have the tool read kernel memory
directly; publishing the standard path is most of the value, because it makes every
*other* Linux tool work too.

### 1. `/sys/bus/usb/devices` + native `lsusb` — **DONE (e98e3de)**

Closest to what already works, so do it first and reuse `lspci` wholesale.
`usbreg.c` already holds everything needed — the boot log prints
`[usbreg] slot=1 port=3 depth=0 speed=2 0461:4141 class=03/01/02 USB HID Mouse (Boot)`.
Publish per-device dirs with `idVendor`, `idProduct`, `bDeviceClass`, `speed`,
`manufacturer`, `product`, `busnum`, `devnum` and a `uevent`. Then a native `lsusb`
printing `Bus %03d Device %03d: ID %04x:%04x %s`.
Note busybox's `lsusb` applet is linked and will take the name until you ship a real
binary — the staging rule skips names a real binary owns, so shipping yours wins.

### 2. SMBIOS + native `dmidecode`

Limine provides an SMBIOS entry-point response; plumb it through the boot info the
way the framebuffer/memmap responses already are. Expose
`/sys/firmware/dmi/tables/smbios_entry_point` and `.../DMI` (both **binary**, so
`sysfs_add_fixed` will not do — you will need a length-carrying variant; the existing
`gen_pci_config` shows the binary-content pattern). Then a native `dmidecode` that
walks the structure table: type 0 (BIOS), 1 (system), 2 (baseboard), 4 (processor),
17 (memory device) covers the useful output. **Real-HW value is high here** — this is
how the EliteDesk will report its actual board and DIMMs.

### 3. hwmon + native `sensors`

Intel core temperature is `IA32_THERM_STATUS` (MSR `0x19C`): bits 22:16 are degrees
*below* `TjMax`, and `TjMax` comes from `MSR_TEMPERATURE_TARGET` (`0x1A2`) bits 23:16.
Per-core is `0x19C` on each CPU; package is `0x1B1`. Publish
`/sys/class/hwmon/hwmon0/` with `name`, `temp1_input` (millidegrees), `temp1_label`,
`temp1_crit`. Then a native `sensors`.
**This is the one most likely to differ on real hardware** — QEMU's MSR emulation may
return 0 or fault. Feature-detect (CPUID.06H:EAX bit 0 = DTS) and print nothing rather
than a fabricated temperature if unavailable. Test on the EliteDesk before claiming it.

### 4. cpufreq + native `cpupower`

`IA32_PERF_STATUS` (`0x198`) bits 15:8 give the current ratio; multiply by the bus
clock (100 MHz on Haswell, from `MSR_PLATFORM_INFO` `0xCE` bits 15:8 for the max
non-turbo ratio). Publish `/sys/devices/system/cpu/cpu<N>/cpufreq/` with
`scaling_cur_freq`, `scaling_min_freq`, `scaling_max_freq`, `scaling_driver`,
`scaling_governor`. Then `cpupower frequency-info`.
**Read-only first.** Setting P-states (`IA32_PERF_CTL` `0x199`) is a separate,
riskier change and must not ride along with the reporting work.

### 5. Editors — decide, don't assume

`nano` (real GNU nano + libncursesw) and `vi` (busybox) both already resolve. A native
terminal editor is a large piece of work with no data-source justification. Before
building one, ask the user whether they want it, and say plainly that two working
editors already ship. If they do want it, `programs/user_gui_edit` is a GUI editor
that already exists and is the wrong starting point for a VT100 one.

## Definition of done for each slice

1. The kernel publishes the data at the **standard Linux path**.
2. A native tool in `programs/<name>/` reads that path and prints the conventional
   format (all four Makefile edits + `program.ld`).
3. A `PKGPROBE_BOOT` probe shows real values from inside the guest, pasted into the
   commit message. **Values, not exit codes.**
4. `lxposix` 32/32, `shparity` 98/98, `oilspec` no new failures, shipped ISO boots
   0 faults.
5. Commit + push per slice, with the traps you hit written into the message.
6. Update `MEMORY.md` and the `linux-userland-commands` memory file.

## Owed, unrelated to this arc, do not lose

- **EliteDesk validation batch.** Untested on real HW: the `e1000e` `RCTL_MPE` fix
  (multicast RX — was blocking all IPv6 RA/NDP), the xHCI SS `bMaxPacketSize0`
  exponent fix, and the USB mouse wheel path. See `real-hardware-elitedesk-bringup`.
- **`net::ERR_SSL_PROTOCOL_ERROR` ×6 on YouTube on real hardware** — TLS is *not*
  broken (the page loads, 9 of 15 requests finish), so this is a subresource-specific
  failure. `logs/cwnet.sh` is the gate, and it deletes `tobyOS.iso`.
- **Audio dead on the EliteDesk**: `[hda] verb timeout` → `codec 0: no Vendor ID
  (resp=0xffffffff)` → 0 codecs.
- **The wake-after-death scheduler flake** (memory `wake-after-death-flake`) — rare,
  mitigated, not root-caused. If a gate goes RED with a 32/32 PASS line, read the
  timestamps before blaming your change.
