#!/bin/bash
# Phase 3 slice 10: build the PID-namespace acceptance test (linux-pidns.elf).
#
# Static glibc, same Bootlin sysroot as programs/linux-cred/build.sh. glibc
# rather than musl because the test drives unshare/clone(CLONE_NEWPID)/kill/procfs
# through syscall(2) and wants a libc whose headers define SYS_* for all of
# them, plus <sys/utsname.h> with the real struct utsname layout.
#
# Usage:   bash programs/linux-pidns/build.sh
# Output:  programs/linux-pidns/linux-pidns.elf   (gitignored; opt-in)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/linux-pidns.elf"

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
  -c "$HERE/main.c" -o "$HERE/linux-pidns.o"

ld.lld -static -o "$OUT" -m elf_x86_64 \
  "$SR/usr/lib/crt1.o" "$SR/usr/lib/crti.o" "$GCC/crtbeginT.o" \
  "$HERE/linux-pidns.o" \
  --start-group \
    "$GCC/libgcc.a" "$GCC/libgcc_eh.a" \
    "$SR/usr/lib/libc.a" "$SR/usr/lib/libc_nonshared.a" \
  --end-group \
  "$GCC/crtend.o" "$SR/usr/lib/crtn.o"

# Brand EI_OSABI = 3 (ELFOSABI_GNU) so the loader picks the Linux personality.
printf '\003' | dd of="$OUT" bs=1 seek=7 count=1 conv=notrunc 2>/dev/null
rm -f "$HERE/linux-pidns.o"
echo "[build.sh] done -> $OUT"
