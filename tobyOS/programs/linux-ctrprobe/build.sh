#!/bin/bash
# Build the in-container probe (linux-ctrprobe.elf) -- the payload that decides
# whether slice 16's capstone passes.
#
# STATIC, and that is load-bearing: this binary is copied INTO the container's
# Alpine rootfs and exec'd after pivot_root, with no host filesystem left to
# resolve a loader from. A dynamic build would need our ld.so inside a rootfs
# that ships musl's.
#
# glibc for the same reason as the sibling tests: it wants SYS_* for everything
# it exercises, including SYS_chmod issued directly (glibc's chmod() routes to
# fchmodat, so the seccomp check must name the syscall it actually makes).
#
# Usage:   bash programs/linux-ctrprobe/build.sh
# Output:  programs/linux-ctrprobe/linux-ctrprobe.elf   (gitignored; opt-in)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/linux-ctrprobe.elf"

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
  -c "$HERE/main.c" -o "$HERE/linux-ctrprobe.o"

ld.lld -static -o "$OUT" -m elf_x86_64 \
  "$SR/usr/lib/crt1.o" "$SR/usr/lib/crti.o" "$GCC/crtbeginT.o" \
  "$HERE/linux-ctrprobe.o" \
  --start-group \
    "$GCC/libgcc.a" "$GCC/libgcc_eh.a" \
    "$SR/usr/lib/libc.a" "$SR/usr/lib/libc_nonshared.a" \
  --end-group \
  "$GCC/crtend.o" "$SR/usr/lib/crtn.o"

# Brand EI_OSABI = 3 (ELFOSABI_GNU) so the loader picks the Linux personality.
printf '\003' | dd of="$OUT" bs=1 seek=7 count=1 conv=notrunc 2>/dev/null
rm -f "$HERE/linux-ctrprobe.o"
echo "[build.sh] done -> $OUT"
