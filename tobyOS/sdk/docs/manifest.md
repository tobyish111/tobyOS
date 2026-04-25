# `tobyapp.toml` manifest reference

Every SDK app ships with a `tobyapp.toml` manifest. `pkgbuild` reads
it, validates it, slurps the listed payloads, and writes a `.tpkg`.

The format is a strict subset of TOML (simpler than the real spec on
purpose -- the parser is a few hundred lines of C and ships in
`tools/pkgbuild.c`).

## Grammar

- **Comments**: `#` to end of line.
- **Sections**: `[app]`, `[install]`, `[files]`, `[launcher]`, `[service]`.
- **Keys**: bare or `"quoted"`.
- **Values**: bare or `"quoted"`. Booleans are bareword `true` / `false`.
- **No** inline tables, arrays, dotted keys, or multi-line strings.

Unknown sections and unknown keys are warnings, not errors --
forward-compatible with future SDK versions adding new fields.

## Sections

### `[app]`  *(required)*

| Key | Type | Required | Default | Notes |
|-----|------|----------|---------|-------|
| `name` | string | yes | -- | matches `<name>.tpkg` filename convention |
| `version` | string | yes | -- | free-form ASCII; recommended: SemVer |
| `kind` | enum | yes | -- | `cli` \| `gui` \| `service` |
| `description` | string | no | "" | one-line summary |
| `author` | string | no | "" | shows up in `pkg info <name>` |
| `license` | string | no | "" | SPDX identifier recommended |

### `[install]`  *(required)*

| Key | Type | Required | Default | Notes |
|-----|------|----------|---------|-------|
| `prefix` | string | yes | -- | **must** start with `/data/`; usually `/data/apps/<name>` |

The kernel installer rejects any package whose file destinations don't
sit under `/data/` -- the rest of the filesystem (kernel ramfs, system
config, users db) is read-only at runtime.

### `[files]`  *(required, at least one entry)*

Each line maps a path **inside the package** to a path **on the host
filesystem** (resolved relative to the manifest's directory by default,
or to `-C <dir>` if passed):

```toml
[files]
"bin/myapp.elf"    = "build/myapp.elf"
"share/data.json"  = "data/data.json"
"share/icon.png"   = "../shared/icons/myapp.png"
```

The full install path is `<install.prefix>/<dest_in_pkg>` -- so the
example above lands files at `/data/apps/myapp/bin/myapp.elf` etc.

### `[launcher]`  *(optional)*

| Key | Type | Required | Notes |
|-----|------|----------|-------|
| `label` | string | yes | text shown under [Apps] |
| `exec` | string | yes | must reference one of the `[files]` entries |
| `icon` | string | no | reserved for future use |

If omitted, the package installs but does not appear in the start menu
(useful for libraries / data packs / services-only packages).

### `[service]`  *(optional, only for `kind = "service"`)*

| Key | Type | Default | Notes |
|-----|------|---------|-------|
| `restart` | enum | `on-failure` | `always` \| `on-failure` \| `never` |
| `autostart` | bool | `false` | start at boot or wait for explicit launch |

The kernel service supervisor (`src/service.c`) honours these values
when restarting the binary after exit. They are encoded in the package
DESC line so the installer can pick them up at install time.

## Example

```toml
# tobyapp.toml -- notes_app

[app]
name        = "notes_app"
version     = "1.0.0"
kind        = "gui"
description = "A simple sticky note for tobyOS"
author      = "tobyOS SDK"
license     = "MIT"

[install]
prefix      = "/data/apps/notes_app"

[files]
"bin/notes_app.elf" = "build/notes_app.elf"
"share/welcome.txt" = "share/welcome.txt"

[launcher]
label = "Notes"
exec  = "bin/notes_app.elf"
```

Validation errors abort `pkgbuild` with a non-zero exit code:

| Code | Meaning |
|------|---------|
| 0 | success |
| 2 | bad command-line / syntax error in manifest |
| 3 | host I/O error (file missing, can't open output, etc.) |
| 4 | manifest validation error (missing required field, bad prefix, ...) |
