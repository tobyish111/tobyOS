#!/bin/bash
# Audio slice 4: stage the UNMODIFIED alsa-lib runtime for the initrd.
#
# Two payloads, both third-party and therefore gitignored (same opt-in
# pattern as programs/chromium/sysroot and programs/busybox):
#
#   libasound.so.2   the real library Chromium dlopens. Taken from the
#                    chromium sysroot, which already carries it.
#   alsa-share/      upstream Debian's /usr/share/alsa config tree, which
#                    alsa-lib parses at snd_pcm_open() time. Extracted from
#                    the libasound2-data deb in the chromium .deb-cache.
#
# asound.conf is OURS (tracked): the site override that points `default`
# at plug:hw:0,0 so the parse does not land on dmix, which needs SysV
# shared memory. See the comments in that file.
#
# Usage: bash programs/alsa/fetch.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
CHROME="$HERE/../chromium"

export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

# ---- the library ----
SRC="$CHROME/sysroot/libasound.so.2.0.0"
[ -f "$SRC" ] || SRC="$CHROME/sysroot/libasound.so.2"
if [ ! -f "$SRC" ]; then
    echo "no libasound in the chromium sysroot -- run programs/chromium/sysroot.sh first"
    exit 1
fi
cp "$SRC" "$HERE/libasound.so.2"
echo "staged libasound.so.2 ($(stat -c%s "$HERE/libasound.so.2") bytes)"

# ---- the config tree ----
DEB="$(ls "$CHROME"/.deb-cache/libasound2-data_*_all.deb 2>/dev/null | head -1)"
if [ -z "$DEB" ]; then
    echo "no libasound2-data deb in $CHROME/.deb-cache -- cannot stage /usr/share/alsa"
    exit 1
fi
TMP="$(mktemp -d)"
cp "$DEB" "$TMP/"
( cd "$TMP" && ar x "$(basename "$DEB")" && tar xf data.tar.xz )
rm -rf "$HERE/alsa-share"
cp -r "$TMP/usr/share/alsa" "$HERE/alsa-share"
rm -rf "$TMP"
echo "staged alsa-share ($(find "$HERE/alsa-share" -type f | wc -l) files)"
