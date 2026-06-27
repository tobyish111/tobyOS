#!/bin/bash
# realtool: stage a REAL off-the-shelf GNU binutils binary (readelf) + the glibc
# runtime it needs, so tobyOS can run an unmodified, non-toy Linux program
# end-to-end. readelf is a genuine ~1.1 MB GNU C program (not written by us):
# getopt_long parsing, file I/O / mmap of the target ELF, the full printf family,
# locale/gettext probing, malloc -- exactly the kind of real libc surface a toy
# proof never touches. It is a dynamic PIE (PT_INTERP=/lib64/ld-linux-x86-64.so.2,
# NEEDED libc.so.6), so it also re-exercises the B26 glibc dynamic-loader path.
#
# Everything is staged under /lib64 + /bin so it never collides with the B26
# /lib staging, and we run it with LD_LIBRARY_PATH=/lib64 for good measure.
#
# Usage:   bash programs/realtool/build.sh
# Output:  programs/realtool/readelf                 (the GNU tool, unmodified)
#          programs/realtool/lib64/ld-linux-x86-64.so.2
#          programs/realtool/lib64/libc.so.6
# (gitignored; opt-in -- the Makefile stages them only when present)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
LIB64="$HERE/lib64"

TCVER="x86-64--glibc--stable-2025.08-1"
for cand in "/c/CustomOS/.glibc-tc/$TCVER" "$HOME/.tobyos-glibc-tc/$TCVER"; do
    [ -d "$cand" ] && TC="$cand" && break
done
[ -n "$TC" ] || { echo "[build.sh] glibc toolchain not found -- run programs/linux-glibc-dyn/build.sh first"; exit 1; }

SR="$TC/x86_64-buildroot-linux-gnu/sysroot"
READELF_SRC="$TC/bin/x86_64-buildroot-linux-gnu-readelf"
[ -f "$READELF_SRC" ] || { echo "[build.sh] missing $READELF_SRC"; exit 1; }

# Ensure the runtime shared objects are present (symlink-less extract may skip).
need_rt() { [ ! -f "$SR/lib/$1" ]; }
if need_rt ld-linux-x86-64.so.2 || need_rt libc.so.6; then
    TB="$(dirname "$TC")/$TCVER.tar.xz"
    [ -f "$TB" ] && ( cd "$(dirname "$TC")" && tar xf "$TB" \
        "$TCVER/x86_64-buildroot-linux-gnu/sysroot/lib/ld-linux-x86-64.so.2" \
        "$TCVER/x86_64-buildroot-linux-gnu/sysroot/lib/libc.so.6" 2>/dev/null || true )
fi

mkdir -p "$LIB64"
cp "$READELF_SRC"               "$HERE/readelf"
cp "$SR/lib/ld-linux-x86-64.so.2" "$LIB64/"
cp "$SR/lib/libc.so.6"            "$LIB64/"

echo "[build.sh] staged unmodified GNU readelf + glibc runtime:"
ls -la "$HERE/readelf" "$LIB64"/*.so* 2>&1
