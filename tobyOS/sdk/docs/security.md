# Security model (Milestone 34 hardening)

This document is the user/operator-facing summary of the hardening
delivered in Milestone 34. It assumes you are familiar with the
package format (see `packaging.md`) and the capability /
sandbox-profile system (see `manifest.md`). For the full
implementation walkthrough see `M34_SUMMARY.md` at the repo root.

The single sentence summary:

> tobyOS verifies every package before it touches the disk, runs every
> app under a least-privilege capability set, blocks writes to
> `/system`, `/boot`, and the package / user databases unless an
> explicitly-privileged code path is active, and surfaces every denial
> through a structured audit log.

---

## 1. Package integrity (M34A)

`.tpkg` files carry SHA-256 digests of (a) the body and (b) every
`FILE` payload. The kernel verifier (`pkg.c::verify_integrity`):

1. Parses the manifest.
2. Re-hashes the body bytes (`body_offset .. EOF`) and compares to
   the manifest `HASH` line.
3. For each declared `FILE`, re-hashes its payload slice and
   compares to the matching `FHASH N` line.
4. Refuses the install if any hash mismatches *before* writing
   anything to disk.

A package without `HASH`/`FHASH` installs only under the WARN
signature policy (default). Set the policy to `REQUIRED` for boxes
where unsigned/unhashed packages must always be rejected.

```c
pkg_set_sig_policy(PKG_SIG_POLICY_REQUIRED);
```

## 2. Update verification (M34B)

`pkg upgrade` and the new `pkg upgrade-path <file>` reuse the same
integrity gate. On any verification failure the upgrade is aborted
**before** the live install is touched, so the previous version
remains the running one. Successful upgrades preserve a `.bak`
rollback blob; `pkg rollback <name>` re-installs from it through the
same verified path.

## 3. Package signing (M34C)

A signed package adds one line:

```
SIG tobyOS-default hmac-sha256:<64-hex-chars>
```

The HMAC is computed over the package body using the symmetric key
stored at `/system/keys/<key-id>.key` on the build host. The kernel
trust store at `/system/keys/trust.db` lists the keys this device
trusts; one line per key:

```
tobyOS-default <64 hex chars>
```

Failure modes:

| Situation                    | Outcome (default policy = WARN) |
|------------------------------|---------------------------------|
| No `SIG`                     | install (with warn)             |
| Trusted `SIG`, valid HMAC    | install                         |
| Trusted `SIG`, bad HMAC      | **reject**                      |
| Unknown key id               | **reject** (even under WARN)    |
| Policy = REQUIRED, no `SIG`  | **reject**                      |

The host tool to sign during build:

```bash
mkpkg --in app.toml --out app.tpkg \
      --sign-key signing.key --key-id tobyOS-default
```

A 32-byte raw key file is the simplest format (`head -c 32 /dev/urandom > signing.key`).

## 4. Sandbox least-privilege defaults (M34D)

App manifests declare the capabilities they need:

```
CAPS FILE_READ,GUI
```

When the launcher spawns the app, the runtime applies the
declaration as a **pure narrowing**:

```
final = inherited & sandbox-profile & declared
```

Available cap names (case-insensitive) match the existing capability
constants:

| Token           | Meaning                                    |
|-----------------|--------------------------------------------|
| `FILE_READ`     | open / readdir / read regular files        |
| `FILE_WRITE`    | create / unlink / mkdir / write files      |
| `EXEC`          | spawn another process                      |
| `NET`           | open sockets, send/receive packets         |
| `GUI`           | open a compositor window                   |
| `SETTINGS`      | read/write the user settings store         |
| `ADMIN`         | (silently dropped from declared lists)     |

Unknown tokens are not silent — they're counted in the parser
output and logged at apply time, so a typo can never widen the
granted mask. `ADMIN` is never grantable via a declared list, so a
malicious or buggy manifest cannot escalate.

## 5. Protected system paths (M34E)

These prefixes are protected at the VFS layer:

```
/system           kernel + system data
/system/keys      trust store
/boot             bootloader / kernel image
/data/packages    install records + .bak rollback blobs
/data/users       user database
```

Any `vfs_create`, `vfs_unlink`, `vfs_mkdir`, `vfs_chmod`,
`vfs_chown`, or `vfs_write` against a protected path returns
`VFS_ERR_PERM` (-12) and emits an audit log line, **unless** the
caller is inside a `sysprot_priv_begin/end` scope. The package
manager and the updater open such a scope around their own writes;
nothing else does.

Reads are not restricted by sysprot — ordinary file modes still
gate them, sandbox capabilities still gate them, but a process that
can read `/system/keys/trust.db` today still can after M34E.

## 6. Audit logging (M34F)

Every security-relevant decision is logged into the existing
structured slog ring (the same ring `logview` and the userland
`tobylog_read` syscall already see). The new tags:

| Tag       | Examples of what's logged                                     |
|-----------|---------------------------------------------------------------|
| `audit`   | login OK / REJECT, pkg install/remove/upgrade/rollback, capability denials |
| `sysprot` | Protected-path write denied / allowed-via-priv                |
| `sec`     | Hash verification failure, signature verification failure     |

Read the audit log from the shell:

```
tobyOS> auditlog              # default subs: audit sysprot sec pkg
tobyOS> auditlog --all -n 50  # everything, latest 50
tobyOS> auditlog --sub=sec    # only signature/hash verifications
tobyOS> auditlog --level=warn # only WARN and ERROR
```

Output format:

```
[<seq>] <ms>ms <LVL> <sub>    pid=<n> <message>
```

Trailing `auditlog: shown=N matched=M total=T dropped=D` line makes
the output script-parseable. The tool is read-only — it never drains
or trims the ring.

## 7. Integrated validation (M34G)

A single shell command exercises every M34 axis end-to-end:

```
tobyOS> securitytest
[securitytest] m34a step 1: PASS (install rc=0)
...
[securitytest] OVERALL: PASS pass=23 fail=0 skip=0 total=23
```

The same code path autoruns at boot when the kernel is built with
`-DSECTEST_AUTORUN` (`make sectest`); see `test_m34g.ps1` for the
QEMU driver.

---

## Quick reference: failure modes

| What you do                                   | What happens                            |
|-----------------------------------------------|-----------------------------------------|
| `pkg install` a tampered `.tpkg`              | Refused; `audit + sec` lines emitted    |
| `pkg upgrade` from a tampered source          | Refused; live install unchanged         |
| `pkg install` a signed package, unknown key   | Refused                                 |
| `pkg install` an unsigned `.tpkg` under REQUIRED | Refused                              |
| App without `NET` cap tries to open a socket  | `EPERM`; `audit cap-deny` line emitted  |
| App without `FILE_WRITE` cap tries to write   | `EPERM`; `audit cap-deny` line emitted  |
| Anything tries `vfs_write` to `/system`       | `VFS_ERR_PERM`; `sysprot deny` line     |
| Failed login (empty/unknown user)             | Refused; `audit login REJECT` line      |

For the full architecture and test results, see `M34_SUMMARY.md`.
