#!/bin/bash
# Build the clone-with-a-child-stack acceptance test (linux-clonestk.elf).
#
# Static glibc, same Bootlin sysroot as programs/linux-clonens/build.sh -- and
# glibc SPECIFICALLY, not musl, because the thing under test IS glibc's clone()
# library wrapper (__clone's `xor %ebp,%ebp; pop %rax; pop %rdi; call *%rax`
# child side). A libc with a different wrapper would test a different bug.
#
# -fno-stack-protector matches the sibling tests. Note that the canary reload is
# exactly what killed Chromium's child, but this test does not need its own
# canary to detect that: bit1 asserts the child's frame is inside the supplied
# stack, which is the cause rather than one of its symptoms.
#
# Usage:   bash programs/linux-clonestk/build.sh
# Output:  programs/linux-clonestk/linux-clonestk.elf   (gitignored; opt-in)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/linux-clonestk.elf"

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
  -c "$HERE/main.c" -o "$HERE/linux-clonestk.o"

ld.lld -static -o "$OUT" -m elf_x86_64 \
  "$SR/usr/lib/crt1.o" "$SR/usr/lib/crti.o" "$GCC/crtbeginT.o" \
  "$HERE/linux-clonestk.o" \
  --start-group \
    "$GCC/libgcc.a" "$GCC/libgcc_eh.a" \
    "$SR/usr/lib/libc.a" "$SR/usr/lib/libc_nonshared.a" \
  --end-group \
  "$GCC/crtend.o" "$SR/usr/lib/crtn.o"

# Brand EI_OSABI = 3 (ELFOSABI_GNU) so the loader picks the Linux personality.
printf '\003' | dd of="$OUT" bs=1 seek=7 count=1 conv=notrunc 2>/dev/null
rm -f "$HERE/linux-clonestk.o"
echo "[build.sh] done -> $OUT"
