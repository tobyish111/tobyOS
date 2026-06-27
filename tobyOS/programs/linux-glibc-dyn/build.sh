#!/bin/bash
# B26: build the REAL glibc-DYNAMIC Linux proof (linux-glibc-dyn.elf) plus stage
# the glibc runtime loader + shared objects it needs at run time.
#
# Like B25 this links against an actual Bootlin glibc sysroot (the in-tree
# freestanding clang cannot supply glibc). Unlike B25 the output is a PIE that
# is *dynamically* linked: PT_INTERP=/lib/ld-linux-x86-64.so.2 and DT_NEEDED
# libc.so.6 / libm.so.6. At run time tobyOS's loader brings in BOTH the PIE and
# the glibc ld.so; ld.so then opens the shared objects from the initrd's /lib.
#
# Usage:   bash programs/linux-glibc-dyn/build.sh
# Output:  programs/linux-glibc-dyn/linux-glibc-dyn.elf       (the PIE)
#          programs/linux-glibc-dyn/lib/ld-linux-x86-64.so.2  (glibc dyn loader)
#          programs/linux-glibc-dyn/lib/libc.so.6             (shared libc)
#          programs/linux-glibc-dyn/lib/libm.so.6             (shared libm)
# (all gitignored; opt-in -- the Makefile stages them only when present)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/main.c"
OUT="$HERE/linux-glibc-dyn.elf"
LIBDIR="$HERE/lib"

# Toolchain cache. Reuse the B25 download locations (either the in-repo
# .glibc-tc probe dir or the build.sh home cache), whichever exists.
TCVER="x86-64--glibc--stable-2025.08-1"
TCURL="https://toolchains.bootlin.com/downloads/releases/toolchains/x86-64/tarballs/$TCVER.tar.xz"
for cand in "/c/CustomOS/.glibc-tc/$TCVER" "$HOME/.tobyos-glibc-tc/$TCVER"; do
    [ -d "$cand" ] && TC="$cand" && break
done
if [ -z "$TC" ]; then
    TCROOT="${GLIBC_TC_ROOT:-$HOME/.tobyos-glibc-tc}"
    echo "[build.sh] fetching glibc sysroot toolchain ($TCVER)..."
    mkdir -p "$TCROOT"
    ( cd "$TCROOT" && curl -fSL -o "$TCVER.tar.xz" "$TCURL" )
    ( cd "$TCROOT" && tar xf "$TCVER.tar.xz" || true )
    TC="$TCROOT/$TCVER"
fi

SR="$TC/x86_64-buildroot-linux-gnu/sysroot"
GCC="$(echo "$TC"/lib/gcc/x86_64-buildroot-linux-gnu/*/)"
TRIPLE=x86_64-unknown-linux-gnu

# Ensure the runtime shared objects are present (they live in the tarball but a
# symlink-less extract can skip them). Pull them straight from the tarball.
need_rt() { [ ! -f "$SR/lib/$1" ]; }
if need_rt ld-linux-x86-64.so.2 || need_rt libc.so.6 || need_rt libm.so.6; then
    TB="$(dirname "$TC")/$TCVER.tar.xz"
    if [ -f "$TB" ]; then
        echo "[build.sh] extracting runtime shared objects from tarball..."
        PFX="$TCVER/x86_64-buildroot-linux-gnu/sysroot/lib"
        ( cd "$(dirname "$TC")" && tar xf "$TB" \
            "$PFX/ld-linux-x86-64.so.2" "$PFX/libc.so.6" "$PFX/libm.so.6" 2>/dev/null || true )
    fi
fi

for f in "$SR/lib/libc.so.6" "$SR/lib/ld-linux-x86-64.so.2" \
         "$SR/usr/lib/Scrt1.o" "$GCC/crtbeginS.o"; do
    [ -f "$f" ] || { echo "[build.sh] missing $f -- toolchain extract failed"; exit 1; }
done

echo "[build.sh] compile (PIC)"
clang --target=$TRIPLE -fPIC -O2 -g0 -fno-stack-protector \
  --sysroot="$SR" -isystem "$SR/usr/include" \
  -c "$SRC" -o "$HERE/linux-glibc-dyn.o"

echo "[build.sh] link (dynamic PIE against real glibc .so.6 files)"
# Link directly against the actual versioned .so files (their DT_SONAME becomes
# our DT_NEEDED). We avoid the sysroot's libc.so/libm.so ld scripts because they
# GROUP /lib64/* paths that don't exist in this symlink-less sysroot.
#
# MSYS2 auto-converts Unix-looking absolute args to Windows paths for the native
# clang/lld. That MUST NOT touch the -dynamic-linker value (it is an in-target
# path baked verbatim into PT_INTERP, NOT a host path) -- otherwise PT_INTERP
# ends up as "C:/msys64/lib/ld-linux-x86-64.so.2". Exclude just that prefix; the
# real host input paths all start with /c/ and still convert normally.
export MSYS2_ARG_CONV_EXCL="/lib/ld-linux-x86-64.so.2"
ld.lld -pie -o "$OUT" -m elf_x86_64 \
  -dynamic-linker /lib/ld-linux-x86-64.so.2 \
  "$SR/usr/lib/Scrt1.o" "$SR/usr/lib/crti.o" "$GCC/crtbeginS.o" \
  "$HERE/linux-glibc-dyn.o" \
  "$SR/lib/libc.so.6" "$SR/lib/libm.so.6" \
  --start-group \
    "$SR/usr/lib/libc_nonshared.a" "$GCC/libgcc.a" "$GCC/libgcc_eh.a" \
  --end-group \
  "$GCC/crtendS.o" "$SR/usr/lib/crtn.o"

# Stage the runtime objects next to the ELF so the Makefile can copy them into
# the initrd /lib. The dynamic loader basename ("ld-linux*") triggers tobyOS's
# Linux-personality auto-detect, so we DON'T need to brand EI_OSABI.
mkdir -p "$LIBDIR"
cp "$SR/lib/ld-linux-x86-64.so.2" "$LIBDIR/"
cp "$SR/lib/libc.so.6"            "$LIBDIR/"
cp "$SR/lib/libm.so.6"            "$LIBDIR/"

rm -f "$HERE/linux-glibc-dyn.o"
echo "[build.sh] done:"
ls -la "$OUT" "$LIBDIR"/*.so* 2>&1
echo "[build.sh] PT_INTERP / NEEDED:"
"$GCC/../../../../bin/x86_64-buildroot-linux-gnu-readelf" -dl "$OUT" 2>/dev/null | grep -E 'INTERP|NEEDED|Requesting' || \
  llvm-readelf -dl "$OUT" 2>/dev/null | grep -E 'INTERP|NEEDED|interpreter' || true
