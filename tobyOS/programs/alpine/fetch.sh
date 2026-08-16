#!/bin/bash
# Fetch the Alpine Linux minirootfs the Linux arc runs on.
#
# Third-party binary, so it is FETCHED and gitignored rather than committed --
# the same opt-in pattern as programs/busybox, programs/chromium/sysroot.sh and
# programs/alsa/fetch.sh. Without it the Makefile simply does not stage
# /alpine.tar.gz, and two milestones go quiet:
#
#   LXALPINE_BOOT     (slice 7)  -- Alpine's own busybox + apk, non-root
#   LXCONTAINER_BOOT  (slice 16) -- the OCI container capstone
#
# Usage:  bash programs/alpine/fetch.sh
# Output: programs/alpine/alpine-minirootfs-3.19.0-x86_64.tar.gz
#
# The checksum is not decoration. The whole point of slice 7 is that tobyOS runs
# a real distro's UNMODIFIED userland, and slice 16's container asserts it is
# running Alpine's OWN busybox by its version banner -- neither claim survives a
# tarball nobody verified. A mismatch is fatal, not a warning.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

VER=3.19.0
BRANCH=v3.19
ARCH=x86_64
FILE="alpine-minirootfs-${VER}-${ARCH}.tar.gz"
URL="https://dl-cdn.alpinelinux.org/alpine/${BRANCH}/releases/${ARCH}/${FILE}"
SHA256=f2b6248c9cbcb0993744d86ca9f67a058706f4ef5bf8c3c47d1cca8acf2013e0

OUT="$HERE/$FILE"

verify() {
    local got
    got="$(sha256sum "$1" | awk '{print $1}')"
    [ "$got" = "$SHA256" ]
}

if [ -f "$OUT" ] && verify "$OUT"; then
    echo "[fetch.sh] $FILE already present and verified"
    exit 0
fi

echo "[fetch.sh] downloading $URL"
if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 -o "$OUT.part" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$OUT.part" "$URL"
else
    echo "[fetch.sh] neither curl nor wget is on PATH"
    exit 1
fi

if ! verify "$OUT.part"; then
    echo "[fetch.sh] SHA-256 MISMATCH -- refusing to stage it."
    echo "[fetch.sh]   want $SHA256"
    echo "[fetch.sh]   got  $(sha256sum "$OUT.part" | awk '{print $1}')"
    rm -f "$OUT.part"
    exit 1
fi

mv "$OUT.part" "$OUT"
echo "[fetch.sh] done -> $OUT ($(stat -c%s "$OUT") bytes, sha256 verified)"
