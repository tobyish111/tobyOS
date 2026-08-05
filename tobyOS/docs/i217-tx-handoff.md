# Handoff: EliteDesk I217 TX / "no network on real hardware"

**Read this whole file before writing any code. The headline is that this
is probably NOT an open driver bug** — it was root-caused in July and
resolved with a BIOS setting, and the driver already prints the diagnosis.
Your first job is to find out whether you are looking at the same fault or
a new one. Do not start a driver arc until that question is answered.

---

## 1. What is actually known

**The symptom, 2026-08-04:** on the HP EliteDesk 800 G1, clicking the
Chromium taskbar pin opens the browser, chrome bootstraps fine, frames
paint — and the page is chrome's error page:

```
[net] FAILED #1: net::ERR_INTERNET_DISCONNECTED
[cwping] u=chrome-error://chromewebdata/
net{req=0 media=0 resp=0 fin=1 fail=1}      blen=129
```

That is a *chrome-level* observation: it means the guest had no usable
route. It does **not** by itself say the NIC is broken. Nobody has yet
looked at that boot's network section — the serial capture started at
1052 s, long after DHCP.

**The July history (memory topic `real-hardware-elitedesk-bringup`):**

- The NIC is an **I217-LM, `8086:153A`**, PCH-integrated LOM, driven by
  `src/e1000e.c`. MAC `64:51:06:46:01:0d`.
- TX was dead with a very specific signature: **TDH pinned at 0 while TDT
  walked away, TX good = 0, RX good > 0.** DHCP DISCOVERs died in the ring.
  (The "gateway cached" ARP entry was learned *passively* from RX, which
  made it look briefly like the stack was working.)
- Commit `f200f28` added the Linux `ich8lan` init bits (TXDCTL DMA-burst,
  TARC0/TARC1, CTRL_EXT b22, LAN_INIT_DONE wait). **Field log proved this
  INSUFFICIENT**: every register read back exactly as written
  (`TXDCTL=0x0141011f TARC0=0x0d800403 TARC1=0x45000403`) and TDH still
  never moved — so the gate was *below* the descriptor ring.
- **ROOT CAUSE: Intel AMT / the Management Engine.** `FWSM=0xe001c24c`
  (bit 15 `FW_VALID` = 1 = ME firmware running). On a vPro box the ME owns
  the MAC↔PHY interconnect for out-of-band management: RX is delivered to
  the OS, but the OS driver's TX never reaches the wire.
- **RESOLUTION: the user disabled Intel AMT in the BIOS and got a DHCP
  lease immediately.** Zero code. The register work in `f200f28` is still
  correct and necessary — just not sufficient on its own.

**The driver already diagnoses this.** `src/e1000e.c:556-583` reads FWSM at
probe and prints:

```
[e1000e] NOTE: Intel ME/AMT firmware is ACTIVE (FWSM.FW_VALID=1). If TX is
         dead (TDH stuck at 0, ...), DISABLE Intel AMT/ME in the BIOS to
         give TX to the OS.
```

So a boot log answers the question by itself.

## 2. Your first move — one boot, no code

Capture the **whole** boot over serial (`plink -serial COM4 -sercfg
38400,8,n,1,N`), from power-on, and grep these, in order:

| grep | what it tells you |
|---|---|
| `[e1000e] probing` / `bound` | which NIC bound, and that it is the I217 (`8086:153a`) and not `nic_igb` (see §4) |
| `[e1000e] NOTE: Intel ME/AMT` | **if present, you are done — AMT is on again. Turn it off in the BIOS.** |
| `[e1000e] stats` | the decisive counters: `TX(good=..)` and `link=` |
| `[e1000e] tx: engine presumed dead` | the driver's own TX-stall detector fired |
| `[dhcp] DISCOVER` / `[dhcp] REQUEST` | did DHCP even get to transmit, and did anything come back |

**Decision rule:**

- `NOTE: Intel ME/AMT` present → same fault, BIOS fix, close it out. Confirm
  by rebooting with AMT off and watching for a lease. **Nothing to code.**
- No AMT note, `TX(good=0)`, TDH pinned → a NEW TX fault with the old
  signature but a different cause. Now §3 applies.
- `TX(good>0)` and DHCP DISCOVERs go out but nothing returns → **not a TX
  bug at all**; it is upstream (link partner, cable, VLAN, DHCP server) or
  RX-side. Re-aim before touching TX.
- Different NIC bound entirely → §4.

## 3. If, and only if, it is a genuinely new TX fault

The July arc already burned these; do not repeat them:

- **Register programming is not the gate.** Every relevant TX register was
  verified to read back exactly as written while TDH stayed 0. Re-deriving
  the ich8lan init sequence is re-doing solved work.
- **EEE / K1 power management was a red herring.** Chased, disproved.
- **A bare `CTRL.RST` hangs the CPU** on this part. The working sequence is
  the "PCH safe reset": disable GIO master, *then* reset.

What has NOT been instrumented, and is where a new fault would show:

1. **Does the descriptor the NIC is asked to fetch actually contain what we
   think?** Dump the TX descriptor (addr/len/cmd/status) *and* the physical
   address it points at, right before bumping TDT — a wrong DMA address is
   invisible in the register readback.
2. **Is the buffer physical address one the device can reach?** This box
   has 8 GB; confirm the TX buffers are below any DMA limit and that the
   HHDM→phys translation used for the descriptor is the identity you think
   it is.
3. **Link state at the PHY**, not just `STATUS.LU` — read the PHY over
   MDIO and confirm speed/duplex negotiated.

## 4. Adjacent, already-settled facts (do not re-chase)

- **`nic_igb` is dead and it is not worth reviving**: RX is broken there;
  `e1000`/`e1000e` is the path that does DHCP. Memory topic `nic-igb-dead-incomplete`.
- `/data` on this box is the user's **Windows disk** — no tobyfs, so `/data`
  falls back to the 8 MiB RAM volume. That is expected, not a fault. (As of
  slice 120 the kernel also relaxes `/data` to 0777 at mount so non-root
  sessions can write it.)
- The PS/2 mouse NACKs at init on this board and that is harmless.

## 5. What "fully closed out" means here

1. The boot log question in §2 is answered, and the answer is written into
   the memory topic `real-hardware-elitedesk-bringup` — **including
   correcting its index line, which still says "OPEN: I217 TX dead" even
   though the body records the July resolution.** That stale line is what
   made this look like an open arc in the first place.
2. Real hardware gets a DHCP lease and holds it.
3. Chromium, launched from the taskbar pin, loads a real web page over the
   network (the local home page at `file:///etc/start.html` already works
   offline — that is not the test).
4. If the resolution is a BIOS setting rather than code, say so plainly in
   the ledger and the memory topic. A zero-code resolution is a legitimate
   close-out, and pretending otherwise invites someone to "fix" it again.

## 6. Method notes that apply to this box specifically

- **Do not blind-fix real-HW-only failures.** A blind PAT guess once
  triple-faulted this machine. Every change costs a full flash + boot cycle.
- Flash with **Rufus in DD/Image mode** (Limine isohybrid; ISO mode mangles
  the layout).
- At 38400 baud any synchronous `kprintf` on a hot path is a real-HW-only
  perf cliff QEMU hides completely — if you add TX-path logging, keep it
  off the per-packet path or rate-limit it hard.
- QEMU cannot reproduce this class at all: it has no ME, no PCH LOM, and
  its e1000 TX always works. **Every QEMU run in this project also
  auto-logs-in as root**, which is how slice 120's `/data` permission bug
  hid for ~80 slices. Real-HW validation is the only configuration that
  exercises these.
