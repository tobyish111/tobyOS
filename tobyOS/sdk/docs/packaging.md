# Packaging and installation

The tobyOS package format is `.tpkg` -- a flat, line-oriented archive
read by the in-kernel `pkg install` command. The SDK ships
`tools/pkgbuild` to produce them from a manifest.

## File format

```
TPKG 1                          <-- magic + format version
NAME hello_cli                  <-- package name
VERSION 1.0.0                   <-- free-form ASCII
PUBLISHER tobyos.org            <-- M34A, optional, free-form ASCII
DESC kind=cli|...               <-- optional, structured by SDK
APP Hello CLI|/data/apps/hello_cli/bin/hello_cli.elf   <-- 0 or more
CAPS FILE_READ,GUI              <-- M34D, optional, declared capabilities
FILE /data/apps/hello_cli/bin/hello_cli.elf 23456      <-- 1 or more
HASH sha256:<hex>               <-- M34A, body SHA-256 (recommended)
FHASH 0 sha256:<hex>            <-- M34A, per-FILE SHA-256, indexed
FHASH 1 sha256:<hex>
SIG <key-id> hmac-sha256:<hex>  <-- M34C, optional package signature
BODY
<raw bytes for FILE 1>          <-- concatenated payloads
<raw bytes for FILE 2>
...
```

Each `FILE` line declares an absolute install path under `/data/` and
the byte count that follows in the body. Payloads are concatenated in
declaration order. The kernel reader rejects packages whose declared
sizes don't match the body length.

### Security headers (Milestone 34)

| Field        | Required? | Purpose                                              |
|--------------|-----------|------------------------------------------------------|
| `PUBLISHER`  | optional  | Free-form publisher / source string (M34A)           |
| `HASH`       | optional* | SHA-256 of the package body (M34A)                   |
| `FHASH N`    | optional* | SHA-256 of FILE *N* (M34A)                           |
| `CAPS`       | optional  | Comma-separated declared capabilities (M34D)         |
| `SIG`        | optional  | `<key-id> hmac-sha256:<hex>` HMAC over the body (M34C) |

\* The default signature policy is `WARN`: missing `HASH`/`FHASH`
installs with a warning. Setting `pkg_set_sig_policy(PKG_SIG_POLICY_REQUIRED)`
flips the policy to fail-closed — both signing and integrity headers
become mandatory.

If a `HASH` or `FHASH` line is present, the kernel **always** verifies
it before any disk write, regardless of policy. A mismatched hash is
**always** rejected. The integrity check runs first; only if it
passes does the signature check (if any) get its turn.

`pkgbuild` (and the underlying `mkpkg`) emits all five M34 fields
automatically when given the relevant flags:

```bash
mkpkg --in tobyapp.toml --out build/myapp.tpkg \
      --publisher tobyos.org \
      --caps FILE_READ,GUI \
      --sign-key keys/signing.key --key-id tobyOS-default
```

The corresponding trust store entry (`/system/keys/trust.db`) is
checked at install time; an unknown key id is rejected even if the
HMAC verifies, so revocation is just "delete the line and reboot".

## Building a `.tpkg`

```bash
$TOBYOS_SDK/tools/pkgbuild tobyapp.toml -o build/myapp.tpkg
```

Useful options:

| Flag | Meaning |
|------|---------|
| `-o <path>` | output path (default: `<name>.tpkg`) |
| `-C <dir>`  | chdir before resolving file sources (default: manifest's directory) |
| `-v`        | verbose -- print each file copied |
| `-h`        | help |

## Installing a `.tpkg`

On a running tobyOS system:

```
$ pkg install /data/repo/myapp.tpkg
[pkg] install OK: name=myapp version=1.0.0 files=1 apps=1
```

You can run this from either the kernel serial shell or the GUI
terminal app. The GUI terminal normally runs as your logged-in user
(uid 1000 for `toby`, 1001 for `guest`), but `pkg install`,
`pkg remove`, `pkg upgrade`, and `pkg rollback` auto-elevate inside
the kernel for the duration of the call so you get the same behaviour
either way -- you do **not** need to `su root` first. (You can still
`whoami` and `su <user>` in the GUI terminal if you want to switch the
shell's identity for other commands.)

Behind the scenes `pkg install`:

1. Reads the header and validates magic + sizes.
2. Refuses to overwrite a package by the same name (use `pkg remove`
   first for upgrades).
3. Refuses to overwrite a `/data/` file owned by a different package.
4. Writes every `FILE` payload to its declared destination.
5. Registers every `APP` entry as a `*.app` descriptor under
   `/data/apps`.
6. Calls `pkg_refresh_launcher()` so the desktop start menu picks up
   the new entry without a reboot.

## Removing a package

```
$ pkg remove myapp
[pkg] remove OK: unlinked 2 files, 1 launcher entries
```

`pkg remove` walks the install record, unlinks every file, removes
the launcher entry, and refreshes the menu. Files outside `/data/`
are never touched -- the prefix rule guarantees a clean uninstall.

## Distributing packages

Today the supported workflows are:

1. **Bake into the initrd** (vendor distribution). `make sdk-samples`
   already drops `hello_cli.tpkg`, `hello_gui.tpkg`, and
   `notes_app.tpkg` into `/repo/` of the initrd, so they're
   discoverable as `pkg install /repo/<name>.tpkg` immediately after
   boot.

2. **Drop into `/data/repo/`** at runtime. The shell's `pkg install`
   accepts any path; `/data/repo/` is just convention. You can use
   the network shell tools (`net push`) to ship a `.tpkg` from the
   host to a running guest.

3. **HTTP fetch**. `pkg install http://host/path.tpkg` works for
   tobyOS systems with networking; the kernel HTTP client downloads
   the body before installing. Useful for self-hosted package repos.

A first-class package server is on the M34+ roadmap.
