#!/bin/bash
# realcc: stage a REAL, unmodified, off-the-shelf C COMPILER so tobyOS can
# compile a C source into a working executable *in the VM* and then run it --
# genuine self-hosting with a third-party toolchain (a clear step past tobyOS's
# own homegrown `tobycc`/`tcc` ports).
#
# We use the public Alpine Linux build of Fabrice Bellard's TinyCC (tcc 0.9.27):
# a dynamic musl program (tcc -> libtcc.so -> libc.musl). It runs through the
# same musl loader busybox-dyn / CPython use, and -- because it links libtcc.so
# -- it also exercises the multi-DSO musl path end to end.
#
# Usage:   bash programs/realcc/build.sh
# Outputs: programs/realcc/tcc              (UNMODIFIED TinyCC 0.9.27, dyn musl)
#          programs/realcc/libtcc.so        (its shared backend)
#          programs/realcc/tcclib/          (libtcc1.a + runtime objs + include/)
# (gitignored; opt-in -- the Makefile stages them only when present)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
TCCBIN="$HERE/tcc"
LIBTCC="$HERE/libtcc.so"
TCCLIB="$HERE/tcclib"

VER="0.9.27_git20250619-r1"
BASE="https://dl-cdn.alpinelinux.org/alpine/edge/community/x86_64"
EXDIR="$HERE/extract"

if [ -f "$TCCBIN" ] && [ -f "$LIBTCC" ] && [ -f "$TCCLIB/libtcc1.a" ]; then
    echo "[build.sh] tcc + libtcc.so + tcclib already present -- reusing"
else
    rm -rf "$EXDIR"; mkdir -p "$EXDIR"
    for pkg in tcc tcc-libs tcc-libs-static; do
        apk="$HERE/${pkg}-${VER}.apk"
        if [ ! -f "$apk" ]; then
            echo "[build.sh] fetching ${pkg}-${VER}.apk ..."
            curl -fSL -o "$apk" "$BASE/${pkg}-${VER}.apk"
        fi
        # An .apk is a gzip-compressed tar (with .SIGN/.PKGINFO members); tar
        # extracts the real files (usr/...) and ignores the metadata members.
        tar -C "$EXDIR" -xzf "$apk" 2>/dev/null || true
    done
    [ -f "$EXDIR/usr/bin/tcc" ]        || { echo "[build.sh] missing usr/bin/tcc"; exit 1; }
    [ -f "$EXDIR/usr/lib/libtcc.so" ]  || { echo "[build.sh] missing usr/lib/libtcc.so"; exit 1; }
    [ -f "$EXDIR/usr/lib/tcc/libtcc1.a" ] || { echo "[build.sh] missing libtcc1.a"; exit 1; }
    cp "$EXDIR/usr/bin/tcc"       "$TCCBIN"
    cp "$EXDIR/usr/lib/libtcc.so" "$LIBTCC"
    rm -rf "$TCCLIB"; mkdir -p "$TCCLIB"
    cp -r "$EXDIR/usr/lib/tcc/." "$TCCLIB/"
    chmod +x "$TCCBIN"
    rm -rf "$EXDIR"
fi

szbin=$(wc -c < "$TCCBIN")
magic=$(head -c 4 "$TCCBIN" | od -An -tx1 | tr -d ' ')
echo "[build.sh] staged TinyCC: tcc ($szbin bytes, magic=$magic)"
echo "[build.sh] tcclib contents:"; ls -1 "$TCCLIB" | head
[ "$magic" = "7f454c46" ] || { echo "[build.sh] tcc not an ELF -- download failed"; exit 1; }
echo "[build.sh] done."
