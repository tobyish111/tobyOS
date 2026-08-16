#!/bin/bash
# Audio slice 4: build the REAL-alsa-lib proof (linux-alsatest.elf).
#
# A glibc-DYNAMIC PIE, like programs/linux-glibc-dyn (B26), because it needs
# dlopen -- and it needs dlopen because that is how Chromium reaches
# libasound.so.2. Links against the Bootlin glibc sysroot; the in-tree
# freestanding clang cannot supply glibc.
#
# Usage:  bash programs/linux-alsatest/build.sh
# Output: programs/linux-alsatest/linux-alsatest.elf     (gitignored)
#
# The runtime it needs (ld-linux, libc.so.6, libm.so.6) is staged by
# programs/linux-glibc-dyn/build.sh; run that first. libasound.so.2 and the
# /usr/share/alsa config tree come from programs/alsa/fetch.sh.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/main.c"
OUT="$HERE/linux-alsatest.elf"

export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

TCVER="x86-64--glibc--stable-2025.08-1"
for cand in "/c/CustomOS/.glibc-tc/$TCVER" "$HOME/.tobyos-glibc-tc/$TCVER"; do
    [ -d "$cand" ] && TC="$cand" && break
done
if [ -z "$TC" ]; then
    echo "[build.sh] no glibc toolchain -- run programs/linux-glibc-dyn/build.sh first"
    exit 1
fi

SR="$TC/x86_64-buildroot-linux-gnu/sysroot"
GCC="$(echo "$TC"/lib/gcc/x86_64-buildroot-linux-gnu/*/)"
TRIPLE=x86_64-unknown-linux-gnu

for f in "$SR/lib/libc.so.6" "$SR/lib/ld-linux-x86-64.so.2" \
         "$SR/usr/lib/Scrt1.o" "$GCC/crtbeginS.o"; do
    [ -f "$f" ] || { echo "[build.sh] missing $f"; exit 1; }
done

echo "[build.sh] compile (PIC)"
clang --target=$TRIPLE -fPIC -O2 -g0 -fno-stack-protector \
  --sysroot="$SR" -isystem "$SR/usr/include" \
  -c "$SRC" -o "$HERE/linux-alsatest.o"

echo "[build.sh] link (dynamic PIE against real glibc .so.6 files)"
# See linux-glibc-dyn/build.sh: MSYS2 would rewrite the -dynamic-linker
# value into a Windows path, but that string is an IN-TARGET path baked
# verbatim into PT_INTERP, not a host path.
export MSYS2_ARG_CONV_EXCL="/lib/ld-linux-x86-64.so.2"
ld.lld -pie -o "$OUT" -m elf_x86_64 \
  -dynamic-linker /lib/ld-linux-x86-64.so.2 \
  "$SR/usr/lib/Scrt1.o" "$SR/usr/lib/crti.o" "$GCC/crtbeginS.o" \
  "$HERE/linux-alsatest.o" \
  "$SR/lib/libc.so.6" "$SR/lib/libm.so.6" \
  --start-group \
    "$SR/usr/lib/libc_nonshared.a" "$GCC/libgcc.a" "$GCC/libgcc_eh.a" \
  --end-group \
  "$GCC/crtendS.o" "$SR/usr/lib/crtn.o"

rm -f "$HERE/linux-alsatest.o"
echo "[build.sh] done:"
ls -la "$OUT"
llvm-readelf -dl "$OUT" 2>/dev/null | grep -E 'NEEDED|interpreter' || true
