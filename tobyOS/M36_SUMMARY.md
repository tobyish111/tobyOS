# Milestone 36 — Self-hosting (incremental, partial)

**Not full self-hosting.** tobyOS can compile and run a *restricted* C subset using the in-guest **TobyC** tool (`/bin/tobycc`); a full ISO C + libc toolchain (GCC/Clang) is *not* in the guest image for this milestone.

## A — In-OS build workflow

- **tobybuild** (`/bin/tobybuild`): reads `/m36/Tobyfile` (`out=`, `src=`), then runs `/bin/tobycc` (basic dependency: `out` must be produced from `src`). `src` may list **multiple** space-separated paths; order matches `tobycc` argument order.
- **TobyC** concatenates all listed `.c` files, then emits a single static ELF to `-o` (one PT_LOAD, ET_EXEC). Failures are reported to stderr; exit status non-zero on error.

## B — Toolchain (lightweight)

- **TobyC** (stage-1): ELF64 ET\_EXEC, single PT\_LOAD, x86-64: `_start` calls `main`, exit via syscall, `main` is `mov eax, N; ret` for the parsed constant `N`.
- **Multi-file example**: `m36_part2.c` (no `return` token) + `m36_sample.c` (`return 42`) — combined buffer must have exactly one `return` + constant for the entrypoint result (see `parse_return_const` in `programs/tobycc/tobycc.c`).

## C — Core userland built in-OS

- **Not achieved for real `ls`/`cat`/shell** — those remain host/ports or prebuilt sbase in initrd. TobyC cannot compile them today. **Progress** is the **selfhost** path: compile a tiny C sample in `/data` and run it, proving the in-OS compiler + VFS + exec pipeline.

## D — Partial system rebuild in-guest

- **Not a kernel or full userland rebuild.** The automated test re-runs the **selfhost** binary after boot, which recompiles the sample to `/data/self.elf` and execs it — a controlled *subset* demonstration without stopping services or swapping `/bin` wholesale.

## E — Self-host validation (automated)

- Boot with `make m36test` (`-DM36_SELFTEST`): kmain spawns `/bin/selfhosttest` once.
- `selfhosttest` runs:  
  `tobycc -o /data/self.elf /m36/m36_part2.c /m36/m36_sample.c` then `/data/self.elf` — expects **exit 42** and prints `M36: PASS self-host compile+run`.

## QEMU (host, VM-safe)

```powershell
cd c:\CustomOS\tobyOS
make m36test
# Requires disk.img (e.g. `make disk.img`) and tobyOS.iso; default QEMU:
powershell -NoProfile -ExecutionPolicy Bypass -File .\test_m36.ps1
```

Raw QEMU (what the script uses; adjust path to `qemu-system-x86_64.exe`):

```text
"D:\Path\to\qemu-system-x86_64.exe" -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk -smp 4 -serial file:serial.log -no-reboot -no-shutdown -display none
```

## Demo flow (manual, inside QEMU)

1. Edit: `cat /m36/m36_sample.c` and change the `return` value (e.g. to `7`), or create a new file on `/data` with `int main(void) { return N; }` only.
2. Compile: `/bin/tobycc -o /data/s.elf /m36/m36_part2.c /m36/m36_sample.c` (or a single file).
3. Run: `/data/s.elf; echo exit=$?` (should match your constant mod 256 as exit status).

Optional: `/bin/tobybuild /m36/Tobyfile` — same as step 2 when `Tobyfile` matches.

## PASS / FAIL (automated, verified in CI setup)

| Check | Result |
|-------|--------|
| `make m36test` then `test_m36.ps1` greps `M36: PASS` in `serial.log` | **PASS** |

## Known limitations

- TobyC is a *stage-1* demo, not ISO C; extra TUs must not contain a misleading `return` before `main`’s return.
- No in-guest GCC/Clang; sbase/regular utilities are not rebuilt by TobyC in M36.
- No kernel self-rebuild in-guest; ABI preserved by using the same `elf_load_user` path as other static ELFs.
