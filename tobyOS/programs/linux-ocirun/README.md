# linux-ocirun — slice 16's capstone

A minimal OCI runtime, plus the bundle and the in-container probe that decide
whether the capstone passes.

```
programs/linux-ocirun/    the runtime          -> /bin/linux-ocirun
  main.c                  config -> namespaces/cgroup/mounts/seccomp/exec
  json.h                  a strict JSON reader (see its header for why strict)
  syscalls.h              syscall name -> number for the seccomp profile
  config.json             the bundle's config, staged to /oci-bundle/config.json
programs/linux-ctrprobe/  the container's payload -> copied into the rootfs
```

Run it by hand:

```
linux-ocirun <bundle-dir> [container-id]
```

`LXCONTAINER_BOOT` (src/kernel.c) drives the whole thing: it creates a
RAM-backed tobyfs volume, mounts it at `/oci`, extracts the in-tree Alpine 3.19
minirootfs into it, copies the probe in, and runs the bundle.

## Why the rootfs is its own mount

`pivot_root(2)` requires the new root to BE a mount point, and this VFS cannot
bind a subtree to make one — `MS_BIND` re-registers an existing *mount's* ops at
a second path, so binding `/data/alp` onto itself is not expressible. Giving the
container its own volume is what makes a real `pivot_root` possible instead of a
`chroot` wearing its name. The runtime refuses to fall back to `chroot`: see the
comment at the pivot in `container_main`.

## What the capstone actually asserts

Not "a container ran". `linux-ctrprobe` returns an 8-bit mask and every bit
names a value rather than a success — the host-only file must be **gone** while
an Alpine-only file is **readable**, `connect()` must fail with **ENETUNREACH
exactly**, `fork()` must **actually** start returning EAGAIN below `pids.max`,
and the busybox that runs must print **Alpine's** version banner. See that
file's header.

## Stated limits

- `mount(2)` gained `-t proc`, `-t sysfs` and `-t cgroup2`. Each mounts an
  existing stateless singleton at a second point; they are three selections of
  one three-line body, and the bundle exercises the `proc` arm.
- No cgroup namespace exists in this kernel, so a container that mounts
  `cgroup2` sees the whole hierarchy rather than a subtree rooted at its own
  cgroup. The bundle deliberately does not mount it.
- No `tmpfs`. This kernel has no mountable in-memory filesystem (ramfs is the
  initrd singleton), so the bundle's `/dev` mount is **expected** to be refused
  — it is in the config on purpose, to prove the runtime reports what it did not
  apply instead of quietly skipping it.
- One uid/gid mapping line only; `/proc/PID/uid_map` here takes a single entry.
- Namespace `path` (join an existing namespace), seccomp argument filters, and
  the seccomp TRACE/NOTIFY actions are refused rather than approximated.
