#!/bin/bash
# realtools: stage a BATTERY of real, unmodified, off-the-shelf GNU Binutils
# programs (objdump, nm, size, strings, c++filt) + the glibc runtime they need,
# so tobyOS runs a SUITE of genuine third-party tools -- a clear step past the
# single `readelf` of the `realtool` milestone.
#
# These are lifted byte-for-byte from the same Bootlin x86-64 glibc 2.41
# toolchain (GNU Binutils 2.43.1). Each is a dynamic PIE
# (PT_INTERP=/lib64/ld-linux-x86-64.so.2, NEEDED libc.so.6 AND libdl.so.2), so
# the battery re-drives the B26 glibc dynamic-loader path INCLUDING a second
# real shared object (the libdl.so.2 stub) and the heavyweight libbfd surface
# (objdump is a full x86-64 disassembler).
#
# Everything is staged under /lib64 + /bin (same layout as realtool, so the
# runtime overlaps harmlessly) and we run with LD_LIBRARY_PATH=/lib64.
#
# Usage:   bash programs/realtools/build.sh
# Output:  programs/realtools/{objdump,nm,size,strings,c++filt}   (unmodified)
#          programs/realtools/lib64/{ld-linux-x86-64.so.2,libc.so.6,libdl.so.2}
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
PFX="$TC/bin/x86_64-buildroot-linux-gnu-"

# Ensure the runtime shared objects are present (symlink-less extract may skip).
need_rt() { [ ! -f "$SR/lib/$1" ]; }
if need_rt ld-linux-x86-64.so.2 || need_rt libc.so.6 || need_rt libdl.so.2; then
    TB="$(dirname "$TC")/$TCVER.tar.xz"
    [ -f "$TB" ] && ( cd "$(dirname "$TC")" && tar xf "$TB" \
        "$TCVER/x86_64-buildroot-linux-gnu/sysroot/lib/ld-linux-x86-64.so.2" \
        "$TCVER/x86_64-buildroot-linux-gnu/sysroot/lib/libc.so.6" \
        "$TCVER/x86_64-buildroot-linux-gnu/sysroot/lib/libdl.so.2" 2>/dev/null || true )
fi

mkdir -p "$LIB64"
# c++filt has a '+' in the name; map it to "cppfilt" on disk so the initrd path
# is shell-clean, but it stays the genuine GNU c++filt binary.
for t in objdump nm size strings; do
    cp "${PFX}${t}" "$HERE/$t"
done
cp "${PFX}c++filt" "$HERE/cppfilt"

cp "$SR/lib/ld-linux-x86-64.so.2" "$LIB64/"
cp "$SR/lib/libc.so.6"            "$LIB64/"
cp "$SR/lib/libdl.so.2"           "$LIB64/"

echo "[build.sh] staged the GNU Binutils battery + glibc runtime:"
ls -la "$HERE"/objdump "$HERE"/nm "$HERE"/size "$HERE"/strings "$HERE"/cppfilt "$LIB64"/*.so* 2>&1
