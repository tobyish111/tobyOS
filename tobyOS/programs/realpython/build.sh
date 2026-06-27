#!/bin/bash
# realpython: stage a REAL, unmodified, off-the-shelf CPython interpreter so
# tobyOS can run a genuine .py script. We use the public, reproducible
# python-build-standalone (astral-sh) *static musl* build: it is statically
# linked (no DSO version-matching -- runs through the same proven static-Linux
# loader path as the bundled busybox / GNU bash) and ships the full pure-python
# standard library.
#
# To keep the initrd lean we don't stage thousands of stdlib files: we zip the
# pure-python stdlib into a single `python310.zip`. CPython's *built-in*
# zipimport reads it directly (no external library), and we point PYTHONPATH at
# it. So the whole interpreter is exactly TWO files: python3 + python310.zip.
#
# Usage:   bash programs/realpython/build.sh
# Outputs: programs/realpython/python3        (UNMODIFIED static-musl CPython 3.10)
#          programs/realpython/python310.zip  (pure-python stdlib, zipimport)
# (gitignored; opt-in -- the Makefile stages them only when present)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
PYBIN="$HERE/python3"
PYZIP="$HERE/python310.zip"

REL="20260610"
VER="3.10.20"
ASSET="cpython-${VER}+${REL}-x86_64-unknown-linux-musl-install_only_stripped.tar.gz"
# '+' must be percent-encoded in the download URL path.
URL="https://github.com/astral-sh/python-build-standalone/releases/download/${REL}/${ASSET//+/%2B}"

TARBALL="$HERE/$ASSET"
EXDIR="$HERE/extract"

if [ -f "$PYBIN" ] && [ -f "$PYZIP" ]; then
    echo "[build.sh] $PYBIN + $PYZIP already present -- reusing"
else
    if [ ! -f "$TARBALL" ]; then
        echo "[build.sh] fetching static-musl CPython $VER ($ASSET) ..."
        curl -fSL -o "$TARBALL" "$URL"
    fi
    echo "[build.sh] extracting (only the binary + stdlib; symlink warnings on Windows are harmless) ..."
    rm -rf "$EXDIR"; mkdir -p "$EXDIR"
    # We only need the real interpreter file + the pure-python stdlib tree.
    # msys2 tar can't create the bin/ + share/ symlinks (and we don't want
    # them), so extract just these two paths and ignore symlink failures.
    tar -C "$EXDIR" -xzf "$TARBALL" \
        python/bin/python3.10 python/lib/python3.10 || true
    # Layout: python/bin/python3.10 (static), python/lib/python3.10/ (stdlib)
    SRCBIN="$EXDIR/python/bin/python3.10"
    SRCLIB="$EXDIR/python/lib/python3.10"
    [ -f "$SRCBIN" ] || { echo "[build.sh] missing $SRCBIN"; exit 1; }
    [ -d "$SRCLIB" ] || { echo "[build.sh] missing $SRCLIB"; exit 1; }
    cp "$SRCBIN" "$PYBIN"
    chmod +x "$PYBIN"
    echo "[build.sh] zipping pure-python stdlib into $(basename "$PYZIP") ..."
    rm -f "$PYZIP"
    # zipimport expects modules at the archive root (encodings/__init__.py,
    # io.py, ...). Skip the C-extension dir (a static binary can't dlopen .so),
    # the test trees, and build config to keep it small.
    ( cd "$SRCLIB" && zip -q -r -X "$PYZIP" . \
          -x 'lib-dynload/*' -x 'test/*' -x '*/test/*' -x '*/tests/*' \
             -x 'config-*/*' -x '__pycache__/*' -x '*/__pycache__/*' \
             -x 'idlelib/*' -x 'turtledemo/*' -x 'ensurepip/*' \
             -x 'site-packages/*' )
    rm -rf "$EXDIR"
fi

szbin=$(wc -c < "$PYBIN")
szzip=$(wc -c < "$PYZIP")
magic=$(head -c 4 "$PYBIN" | od -An -tx1 | tr -d ' ')
echo "[build.sh] staged CPython: python3 ($szbin bytes, magic=$magic), python310.zip ($szzip bytes)"
[ "$magic" = "7f454c46" ] || { echo "[build.sh] python3 not an ELF -- download failed"; exit 1; }
echo "[build.sh] done."
