# Milestone 34 — Defensive OS Hardening

**Status: COMPLETE** (all seven sub-milestones + integrated `securitytest` pass)

M34 strengthens tobyOS security across the package, update, runtime,
and integrity surfaces, **without** redesigning the OS or adding any
new offensive tooling. Every change is a containment measure: it
either narrows a default, makes a previously implicit check explicit,
or surfaces a previously silent denial in the audit trail.

The milestone is structured as seven phases (M34A → M34G), each with
its own design / implementation / test / validation cycle. A
final integrated harness (`securitytest`) re-exercises every axis from
the live, non-selftest kernel to confirm no regressions.

---

## Implemented security features

### M34A — Package integrity metadata

Every `.tpkg` now carries:

- `PUBLISHER <name>` — free-form publisher / source string,
  truncated to `PKG_PUBLISHER_MAX`.
- `HASH sha256:<hex>` — SHA-256 of the package body
  (`body_offset .. EOF`). Verified before any disk write.
- `FHASH <index> sha256:<hex>` — per-`FILE` SHA-256, indexed by the
  declared FILE order. Verified after the body hash.
- `VERSION <ver>` — already present pre-M34, now consulted by the
  integrity check so a header swap is detectable.

Implementation:

- `src/sec.c` / `include/tobyos/sec.h` — minimal, audited SHA-256 +
  HMAC-SHA256 + hex helpers + constant-time compare. No external
  dependency; ~250 LOC; covered by `sec_selftest` at every boot.
- `src/pkg.c::verify_integrity` — central integrity gate. Called
  from `pkg_install_path` and `apply_replace` (used by upgrade) so
  install and upgrade share one path.
- `tools/mkpkg.c` — host builder rewritten to compute and emit all
  M34A fields. Includes `--corrupt`, `--bad-hash`, `--no-hash`,
  `--bad-fhash` flags so the test fixtures live as a deterministic
  byte sequence on every build.

Failure mode is **fail-closed**: a missing `HASH` is accepted only
under the WARN policy; a wrong `HASH` is always rejected before any
file is written.

### M34B — Update verification

`pkg upgrade` and `pkg upgrade-path` (new in M34B) reuse
`verify_integrity` against the candidate `.tpkg` *before* touching
the live install. The pre-existing rollback semantics from M17 are
unchanged — on any verification or write failure, the in-memory
backup is restored and the user-visible install reverts.

The new `pkg_upgrade_path(const char *src_tpkg)` API lets the test
harness (and a future operator command) drive an upgrade from a
specific source rather than scanning the repo. The implementation
peeks at the source's `NAME`, then dispatches to the same
`upgrade_internal` path the auto-discovery upgrade uses.

### M34C — Package signing groundwork

A simple, in-tree signing model:

- `SIG <key-id> hmac-sha256:<hex>` — optional manifest field. The
  HMAC is computed over the package body using the key registered
  under `<key-id>` in the trust store.
- `/system/keys/trust.db` — text trust store, one
  `key-id <ws> hex-bytes` line per public key. Loaded once at boot
  by `sig_trust_store_init`.
- `pkg_sig_policy` — boot-tunable: `WARN` (unsigned installs with
  warning, default) or `REQUIRED` (unsigned rejected outright).
- Host tooling: `mkpkg --sign-key=<file> --key-id=<id>` produces a
  signed `.tpkg` from a 32-byte key file.

HMAC-SHA256 was chosen as the initial primitive for two reasons:
(1) the same SHA-256 implementation is already in tree for M34A,
making the cryptographic surface minimal and auditable, and (2) the
trust-store + verification plumbing is the actual hard part — once
that lands, swapping HMAC for an asymmetric primitive (Ed25519) is a
local change in `src/sec.c`. A `TODO(M34C-asym)` marker tracks the
upgrade path.

Failure modes:

- Trusted key + valid HMAC → install accepted.
- Trusted key + tampered body or HMAC → rejected, audit-logged.
- Unknown key id → rejected regardless of policy.
- No SIG line → policy-driven (WARN accepts, REQUIRED rejects).

### M34D — Stronger sandbox defaults

Manifests now declare the capabilities each app needs:

```
CAPS FILE_READ,GUI
```

`pkg.c::emit_app_descriptor` propagates the declared list into the
`*.app` descriptor on disk. The launcher (`gui_launch_*` family)
remembers the declared caps per registered entry and forwards them
into `proc_spawn` via the new `proc_spec.declared_caps` field.

`cap.c::cap_apply_declared` then applies the declared list as a pure
narrowing operation:

```
inherited & sandbox-profile & declared
```

Key invariants:

- **Pure narrowing.** Declaring a cap the parent doesn't have can
  never grant it.
- **CAP_ADMIN is never grantable** via a declared list. The parser
  silently strips it for safety.
- **Unknown tokens are not silent.** They're counted in
  `*out_unknown` and logged at apply time so a typo can never
  widen the granted mask.

### M34E — System file protection

A new `sysprot` module designates these prefixes as protected:

| Prefix             | Why                                            |
|--------------------|------------------------------------------------|
| `/system`          | Kernel binaries, system data                   |
| `/system/keys`     | Trust store (subset of `/system`, called out)  |
| `/boot`            | Bootloader + kernel image                      |
| `/data/packages`   | Install records and `.bak` rollback blobs      |
| `/data/users`      | User database                                  |

VFS write paths (`vfs_create`, `vfs_unlink`, `vfs_mkdir`,
`vfs_chmod`, `vfs_chown`, `vfs_write_all`, `vfs_write` on protected
handles) call `sysprot_check_write` before mutating. A denied write
returns `VFS_ERR_PERM` and emits `SLOG_WARN(SLOG_SUB_SYSPROT, ...)`
so the audit log shows exactly who tried what.

Trusted callers (the package manager, the updater, boot-time
helpers) wrap their write region in a paired
`sysprot_priv_begin/end`. The scope is per-process (counter on
`struct proc`), so it nests correctly across helpers and survives
context switches naturally.

A `sysprot_set_test_strict(true)` knob lets the boot self-test drive
the deny path from kernel context (pid 0); it's always false in
production.

### M34F — Audit logging

Every security-relevant event is now emitted into the structured
slog ring with one of three subsystem tags:

| Tag                | Source                                              |
|--------------------|-----------------------------------------------------|
| `SLOG_SUB_AUDIT`   | Package lifecycle, login/session, capability denials |
| `SLOG_SUB_SYSPROT` | Protected-path write denials (and explicit allows)  |
| `SLOG_SUB_SEC`     | Hash + signature verification outcomes              |

Concrete sites wired during M34F:

- `pkg.c`: `install OK`, `install REJECT reason=integrity`,
  `remove OK`, `upgrade OK`, `upgrade REJECT reason=integrity`,
  `rollback OK`, `rollback FAIL`.
- `cap.c::cap_check`: `cap-deny pid=N uid=N '<name>' op=<op> missing=<cap>`.
- `cap.c::cap_check_path`: `sandbox-deny ...path=... root=...`.
- `sysprot.c`: deny + allow lines on protected-path mutations.
- `session.c`: `login OK`, `login REJECT reason=...`, `logout`.
- `sec.c`: hash mismatch, missing key, signature mismatch.

Operator-facing tool: a new `auditlog` shell builtin (`src/shell.c`)
that drains the slog ring and prints AUDIT/SYSPROT/SEC/PKG entries
in the same format as `logview`. Supports:

- `--all` — every subsystem (no audit filter)
- `--sub=NAME` — restrict to one subsystem
- `--level=LVL` — drop records above the chosen severity
- `--since=SEQ` — start from a specific sequence
- `-n N` — keep only the latest N matches

Trailing summary line `auditlog: shown=N matched=M total=T dropped=D`
makes the output script-parseable.

### M34G — Integrated security validation suite

`src/sectest.c` exposes one entry point, `sectest_run`, that
exercises every M34 axis against the **live** production paths (no
selftest gating). It's surfaced two ways:

1. **Shell builtin.** `securitytest` runs the full suite and prints
   PASS/FAIL/SKIP per step, plus a final
   `[securitytest] OVERALL: PASS|FAIL pass=P fail=F skip=S total=T`
   line.
2. **Boot autorun.** Built with `make sectest` (which adds
   `-DSECTEST_AUTORUN`), the kernel calls `sectest_run` from
   `kmain` after `pkg_init`. The PowerShell driver
   `test_m34g.ps1` boots that build, waits for the OVERALL line,
   and exits with the same verdict.

Areas exercised:

| Step group     | Verifies                                                          |
|----------------|-------------------------------------------------------------------|
| `m34a step *`  | Install valid + reject corrupt body                               |
| `m34b step *`  | Install v1 → corrupt v2 rejected (version preserved) → valid v2 → rollback to v1 |
| `m34c step *`  | Trusted accepted, unknown rejected, tampered rejected, REQUIRED rejects unsigned |
| `m34d step *`  | `cap_parse_list` mask + `cap_apply_declared` narrows a real proc + ADMIN never grantable |
| `m34e step *`  | Write to `/data/packages/*` denied (`VFS_ERR_PERM`); priv scope allows |
| `m34f step *`  | Audit emit observable in ring; `pkg install` audit hit            |
| `regression`   | install → upgrade → rollback → remove cycle still green           |

---

## Tests

Two PowerShell drivers, each shipping with the source tree:

| Driver           | What it runs                                          | Build target |
|------------------|-------------------------------------------------------|--------------|
| `test_m34.ps1`   | M34A/B/C/D/E/F selftests inside the kernel boot path  | `make m34test` (`-DPKG_M34_SELFTEST`) |
| `test_m34g.ps1`  | The integrated `securitytest` autorun                 | `make sectest` (`-DSECTEST_AUTORUN`) |

Both drivers boot tobyOS headless under QEMU with serial + debug
captured to disk, wait for a canonical marker line, and exit 0 only
on PASS.

### Final results

```
$ pwsh -File test_m34.ps1
[m34] OVERALL: PASS
   sub-steps: total=42 pass=42 fail=0

$ pwsh -File test_m34g.ps1
[m34g] OVERALL: PASS
   breadcrumbs: total=23 pass=23 fail=0 skip=0
```

Combined: **65 / 65 PASS, 0 FAIL, 0 SKIP.**

---

## QEMU validation commands

The most useful invocations during M34 development:

```bash
# Default build, no test gating. Verifies the production kernel still
# boots cleanly and the new builtins are visible from the shell.
make all
make run                          # interactive QEMU
# At the prompt:
#   tobyOS> securitytest
#   tobyOS> auditlog --all -n 30

# M34A/B/C/D/E/F kernel-side selftest harness.
make m34test                      # rebuilds with -DPKG_M34_SELFTEST
pwsh -File test_m34.ps1           # boots, waits for verdict line

# M34G integrated security validation suite (live paths).
make sectest                      # rebuilds with -DSECTEST_AUTORUN
pwsh -File test_m34g.ps1
```

The PowerShell drivers also accept `-Build` to chain the rebuild
themselves, but on Windows/MSYS2 environments where compiler warnings
are surfaced as PowerShell stderr it's more reliable to run the
`make` step in MSYS2 first and the driver bare.

---

## Known limitations

These are intentional scope-management decisions, not bugs. Each one
is documented inline next to the relevant code.

1. **HMAC-SHA256, not asymmetric crypto.** M34C delivers the package
   format, the trust store, the verification gate, and the host
   tooling. Substituting an asymmetric primitive (Ed25519) is a
   local change in `src/sec.c`; the call sites are stable. Tracked
   by `TODO(M34C-asym)`.
2. **Trust store is text-only.** `/system/keys/trust.db` is a
   line-oriented file. Rotating keys requires editing it and a
   reboot (or a `sig_trust_store_init` re-call). A rotation API
   is out of scope for M34.
3. **`auditlog` is a shell builtin, not a userland binary.** The
   audit ring lives in the kernel and is already drainable from
   userland via `SYS_SLOG_READ`; promoting `auditlog` to a `/bin`
   binary that uses that syscall is a pure userland exercise and
   tracked separately from M34F itself.
4. **`securitytest` is a kernel builtin.** Same reasoning: it
   exercises kernel-internal APIs (`cap_apply_declared`,
   `sysprot_priv_begin`) that are not yet exposed to userland.
   Promoting to a `/bin` tool requires deciding which of those
   APIs deserve syscall-level surfaces.
5. **Read-only `/system` on initrd.** During boot the system
   partition lives on the read-only initrd ramfs; sysprot-protected
   writes there return `VFS_ERR_ROFS` (from the ramfs) before
   sysprot's `VFS_ERR_PERM` can fire. This is fine *as a final-
   state property* (the kernel image really is read-only at
   runtime), but it means the M34E selftest exercises the deny
   path against `/data/packages/`, a writable protected mount
   point on the persistent disk.

---

## Files added or substantially changed

```
NEW   include/tobyos/sec.h           SHA-256 / HMAC-SHA256 / hex / trust store
NEW   include/tobyos/sysprot.h       protected-prefix VFS gate
NEW   include/tobyos/sectest.h       sectest_run public entry
NEW   src/sec.c                      crypto + trust store implementation
NEW   src/sysprot.c                  protected-path enforcement
NEW   src/sectest.c                  M34G integrated validation suite
NEW   test_m34.ps1                   M34A-F kernel selftest driver
NEW   test_m34g.ps1                  M34G integrated suite driver
NEW   M34_SUMMARY.md                 this document

CHG   include/tobyos/pkg.h           PUBLISHER/HASH/FHASH/CAPS/SIG fields,
                                     pkg_sig_policy enum, pkg_upgrade_path,
                                     pkg_get_installed_version
CHG   include/tobyos/cap.h           cap_parse_list + cap_apply_declared
CHG   include/tobyos/proc.h          proc_spec.declared_caps,
                                     struct proc.sysprot_priv
CHG   include/tobyos/vfs.h           struct vfs_file.sysprot
CHG   include/tobyos/slog.h          SLOG_SUB_AUDIT / SYSPROT / SEC tags
CHG   include/tobyos/gui.h           launcher caps registration / lookup

CHG   src/pkg.c                      verify_integrity, pkg_upgrade_path,
                                     pkg_m34_selftest (A-F), audit emission
CHG   src/cap.c                      capability + sandbox denial audit
CHG   src/proc.c                     declared-caps narrowing in proc_spawn
CHG   src/gui.c                      launcher tracks per-entry caps
CHG   src/vfs.c                      sysprot integration on every write op
CHG   src/session.c                  login / logout audit emission
CHG   src/kernel.c                   sec_selftest, sig_trust_store_init,
                                     sysprot_init, sectest_run autorun
CHG   src/shell.c                    auditlog + securitytest builtins
CHG   tools/mkpkg.c                  M34 fields, signing flag, fixture options
CHG   Makefile                       sec.c / sysprot.c / sectest.c, fixtures,
                                     m34test + sectest targets, MKPKG shim
```

---

## Final checklist

- [x] Package integrity metadata (M34A) — implemented + tested
- [x] Update verification preserves rollback (M34B) — implemented + tested
- [x] Package signing format + trust store + host tooling (M34C) — implemented + tested
- [x] Sandbox least-privilege defaults via declared CAPS (M34D) — implemented + tested
- [x] Protected critical OS paths (M34E) — implemented + tested
- [x] Audit logging + `auditlog` operator tool (M34F) — implemented + tested
- [x] Integrated `securitytest` validation harness (M34G) — implemented + tested
- [x] Default build still passes a clean boot
- [x] No offensive tooling introduced
- [x] No previously-existing security control weakened
- [x] All tests run inside tobyOS / QEMU
- [x] Documentation updated (this file + `sdk/docs/packaging.md`)
