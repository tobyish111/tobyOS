#!/bin/bash
# B25: build the REAL glibc-static Linux proof (linux-glibc.elf).
#
# This program is NOT built by the in-tree freestanding clang -- it links
# against an actual glibc (libc.a + crt objects + libgcc_eh.a). We get those
# from a prebuilt Bootlin x86-64 glibc toolchain (just used as a sysroot; its
# gcc binaries are Linux ELFs we never run -- the in-tree Windows clang/lld do
# the compile+link). The resulting ELF is branded EI_OSABI=GNU so the loader
# selects the Linux personality.
#
# Usage:   bash programs/linux-glibc/build.sh
# Output:  programs/linux-glibc/linux-glibc.elf  (gitignored; opt-in)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/main.c"
OUT="$HERE/linux-glibc.elf"

# Cache the toolchain outside the repo so it isn't re-downloaded each build.
TCROOT="${GLIBC_TC_ROOT:-$HOME/.tobyos-glibc-tc}"
TCVER="x86-64--glibc--stable-2025.08-1"
TCURL="https://toolchains.bootlin.com/downloads/releases/toolchains/x86-64/tarballs/$TCVER.tar.xz"
TC="$TCROOT/$TCVER"

if [ ! -d "$TC" ]; then
    echo "[build.sh] fetching glibc sysroot toolchain ($TCVER)..."
    mkdir -p "$TCROOT"
    ( cd "$TCROOT" && curl -fSL -o "$TCVER.tar.xz" "$TCURL" )
    # The .so/lib64 symlinks may fail to extract on filesystems without symlink
    # support (e.g. msys2 default); that's fine -- we only need the .a/.o files.
    ( cd "$TCROOT" && tar xf "$TCVER.tar.xz" || true )
fi

SR="$TC/x86_64-buildroot-linux-gnu/sysroot"
GCC="$(echo "$TC"/lib/gcc/x86_64-buildroot-linux-gnu/*/)"
TRIPLE=x86_64-unknown-linux-gnu

for f in "$SR/usr/lib/libc.a" "$SR/usr/lib/crt1.o" "$GCC/libgcc.a"; do
    [ -f "$f" ] || { echo "[build.sh] missing $f -- toolchain extract failed"; exit 1; }
done

echo "[build.sh] compile"
clang --target=$TRIPLE -static -O2 -g0 -fno-stack-protector \
  --sysroot="$SR" -isystem "$SR/usr/include" \
  -c "$SRC" -o "$HERE/linux-glibc.o"

echo "[build.sh] link (static glibc + libgcc[_eh] + pthread/m folded in)"
# libm.a is an ld script GROUPing /usr/lib64/* paths that may not exist on a
# symlink-less extract, so reference the real archives directly. --start-group
# resolves the crt/libc/libgcc cycle.
ld.lld -static -o "$OUT" -m elf_x86_64 \
  "$SR/usr/lib/crt1.o" "$SR/usr/lib/crti.o" "$GCC/crtbeginT.o" \
  "$HERE/linux-glibc.o" \
  --start-group \
    "$GCC/libgcc.a" "$GCC/libgcc_eh.a" \
    "$SR/usr/lib/libc.a" "$SR/usr/lib/libc_nonshared.a" \
    "$SR/usr/lib/libpthread.a" \
    "$SR/usr/lib/libm-2.41.a" "$SR/usr/lib/libmvec.a" \
  --end-group \
  "$GCC/crtend.o" "$SR/usr/lib/crtn.o"

# Brand EI_OSABI = 3 (ELFOSABI_GNU) for deterministic Linux-personality pickup.
printf '\003' | dd of="$OUT" bs=1 seek=7 count=1 conv=notrunc 2>/dev/null
rm -f "$HERE/linux-glibc.o"

echo "[build.sh] done -> $OUT"
ls -la "$OUT"
