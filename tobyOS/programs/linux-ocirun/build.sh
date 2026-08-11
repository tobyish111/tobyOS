#!/bin/bash
# Build the minimal OCI runtime (linux-ocirun.elf) -- slice 16's capstone.
#
# Static glibc, same Bootlin sysroot as the sibling namespace tests. Static so
# the runtime keeps working after pivot_root: a dynamic build would need its
# loader and libc to exist inside the CONTAINER's rootfs, which would make the
# runtime a dependency of every bundle it starts.
#
# glibc rather than musl because the runtime drives the container through
# glibc's clone(fn, child_stack, flags, arg) library function -- the same entry
# point Chromium's sandbox uses, and the one whose child_stack argument this
# kernel used to ignore.
#
# Usage:   bash programs/linux-ocirun/build.sh
# Output:  programs/linux-ocirun/linux-ocirun.elf   (gitignored; opt-in)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/linux-ocirun.elf"

TCROOT="${GLIBC_TC_ROOT:-$HOME/.tobyos-glibc-tc}"
TCVER="x86-64--glibc--stable-2025.08-1"
TC="$TCROOT/$TCVER"
# The repo-local cache the slice-1 work already populated is also honoured.
[ -d "$TC" ] || TC="$(cd "$HERE/../../.." && pwd)/.glibc-tc/$TCVER"
[ -d "$TC" ] || { echo "[build.sh] no glibc sysroot; run programs/linux-glibc/build.sh first"; exit 1; }

SR="$TC/x86_64-buildroot-linux-gnu/sysroot"
GCC="$(echo "$TC"/lib/gcc/x86_64-buildroot-linux-gnu/*/)"

clang --target=x86_64-unknown-linux-gnu -static -O2 -g0 -fno-stack-protector \
  --sysroot="$SR" -isystem "$SR/usr/include" \
  -c "$HERE/main.c" -o "$HERE/linux-ocirun.o"

ld.lld -static -o "$OUT" -m elf_x86_64 \
  "$SR/usr/lib/crt1.o" "$SR/usr/lib/crti.o" "$GCC/crtbeginT.o" \
  "$HERE/linux-ocirun.o" \
  --start-group \
    "$GCC/libgcc.a" "$GCC/libgcc_eh.a" \
    "$SR/usr/lib/libc.a" "$SR/usr/lib/libc_nonshared.a" \
  --end-group \
  "$GCC/crtend.o" "$SR/usr/lib/crtn.o"

# Brand EI_OSABI = 3 (ELFOSABI_GNU) so the loader picks the Linux personality.
printf '\003' | dd of="$OUT" bs=1 seek=7 count=1 conv=notrunc 2>/dev/null
rm -f "$HERE/linux-ocirun.o"
echo "[build.sh] done -> $OUT"
