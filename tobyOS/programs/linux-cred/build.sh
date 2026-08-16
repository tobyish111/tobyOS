#!/bin/bash
# Linux slice 2: build the POSIX-credentials acceptance test (linux-cred.elf).
#
# Static glibc, same Bootlin sysroot as programs/linux-glibc/build.sh -- glibc
# rather than musl because the test drives capget/capset and setres*id through
# syscall(2) and wants a libc whose headers define SYS_* for all of them.
#
# Usage:   bash programs/linux-cred/build.sh
# Output:  programs/linux-cred/linux-cred.elf   (gitignored; opt-in)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/linux-cred.elf"

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
  -c "$HERE/main.c" -o "$HERE/linux-cred.o"

ld.lld -static -o "$OUT" -m elf_x86_64 \
  "$SR/usr/lib/crt1.o" "$SR/usr/lib/crti.o" "$GCC/crtbeginT.o" \
  "$HERE/linux-cred.o" \
  --start-group \
    "$GCC/libgcc.a" "$GCC/libgcc_eh.a" \
    "$SR/usr/lib/libc.a" "$SR/usr/lib/libc_nonshared.a" \
  --end-group \
  "$GCC/crtend.o" "$SR/usr/lib/crtn.o"

# Brand EI_OSABI = 3 (ELFOSABI_GNU) so the loader picks the Linux personality.
printf '\003' | dd of="$OUT" bs=1 seek=7 count=1 conv=notrunc 2>/dev/null
rm -f "$HERE/linux-cred.o"
echo "[build.sh] done -> $OUT"
