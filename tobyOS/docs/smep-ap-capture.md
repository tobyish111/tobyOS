# Capturing the AP / argc≥1 / SMAP SMEP fault on real hardware

**Why:** the multi-core "argc≥1 first-run proc on a secondary CPU under SMAP
SMEP-faults" bug is **not reproducible in QEMU** — tried `qemu64,+smep,+smap`,
the same with argv dereferenced, and `Skylake-Client,+smep,+smap` under
multi-threaded TCG (`-accel tcg,thread=multi`, true concurrent host threads).
All clean: 32/32 workers, 0 faults, healthy desktop. (No KVM on the Windows
host, so MTTCG is the most faithful concurrency model we can run locally.)

To root-cause it we need the actual fault frame from the Skylake EliteDesk,
captured over the COM1 null-modem cable.

## What's instrumented

- **`-DMCARGV_BOOT`** (kernel.c): after boot completes it runs 8 rounds × 4
  `argc=4 / envc=3` `/bin/mctest` workers spawned together, so APs steal and
  first-run them. `/bin/mctest` now dereferences `argv[]` (touches the packed
  user-stack pointers — the region the fault was reported to land in). If the
  bug is live, round 1 should trip it.
- **SMEP fault dump** (isr.c `default_exception`, always compiled in): on a
  supervisor-mode #PF with the instruction-fetch bit (`vector==14 &&
  !from_user && err&0x10`) it `cli`s the faulting CPU and prints, bracketed by
  greppable banners:
  ```
  ===SMEP-FAULT-BEGIN===
    [SMEP] cpu=<n> pid=<p> '<name>' is_idle=.. is_thread=..
    [SMEP] user_entry=.. user_rsp=..
    [SMEP] kstack_top=.. saved_rsp=.. proc.cr3=..
    [SMEP] fault_rip - user_rsp   = ..
    [SMEP] fault_rip - user_entry = ..
  ===SMEP-FAULT-END===
  ```
  then the normal `*** EXCEPTION 14 ***` reg dump and `KERNEL PANIC` banner.

## Build the diagnostic image

From PowerShell in `c:\CustomOS\tobyOS` (toolchain on PATH, native TMP — see
`memory/tobyos-build-env`):

```powershell
$env:Path = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + $env:Path
$env:TMP="C:\CustomOS\tobyOS\.tmpbuild"; $env:TEMP=$env:TMP; $env:TMPDIR=$env:TMP
make EXTRA_CFLAGS="-DMCARGV_BOOT"
```

Do **not** add `-DFAST_BOOT/-DQUICK_BOOT` — we want the real, full boot path
(the same one that faults). Burn / dd `tobyOS.iso` to the boot USB as usual.

## Serial line settings (both ends)

COM1 (I/O port 0x3F8) is brought up at **38400 baud, 8 data bits, no parity,
1 stop bit (8N1), no flow control**, polled TX (serial.c — divisor = 3, so
115200 ÷ 3 = 38400). Set the capture terminal on the *other* end of the
null-modem cable identically. **Wrong baud = garbage; 38400 is not the common
115200 default**, so double-check this.

- **PuTTY**: Connection → Serial, COMx, **38400**, 8, None, 1, Flow control =
  None. Session → Logging → "All session output" → pick a file.
- **`plink`** (scriptable): `plink -serial COM3 -sercfg 38400,8,n,1,N | tee capture.log`
- **Linux capture box**: `screen /dev/ttyUSB0 38400` (log with `Ctrl-a H`),
  or `picocom -b 38400 /dev/ttyUSB0`.

(If you'd rather capture at 115200, change serial.c:33 `outb(COM1_DATA, 0x01)`
— divisor 1 = 115200 — and rebuild. The 38400 default is what the current
image emits.)

## Run + collect

1. Start the capture terminal/logger first.
2. Boot the EliteDesk from the diagnostic USB.
3. Watch for either:
   - `MCARGV: all argc>=1 workers completed (no fault)` → did **not** repro
     (tell me — that itself is informative), or
   - a `===SMEP-FAULT-BEGIN===` block followed by the panic → **that's the
     data we need.** Grab the whole block + the `*** EXCEPTION 14 ***` reg
     dump + the `KERNEL PANIC` reg dump.
4. Paste the captured text back. The fault RIP vs `user_rsp`/`user_entry`,
   which CPU, and `saved_rsp` pin down whether it's the `proc_context_switch`
   `ret`, the `iretq` frame build, or a stale per-CPU TSS/GS/syscall_rsp.

Note: the kernel also UDP-uploads the bootlog (`[bootlog] net upload`) if the
LAN collector is reachable, so a panic dump may also land there — but the
serial capture is the reliable channel for a fault that halts the box.
