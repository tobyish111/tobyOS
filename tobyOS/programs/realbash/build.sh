#!/bin/bash
# realbash: stage a REAL, unmodified, off-the-shelf GNU bash so tobyOS can run it
# as an INTERACTIVE shell over the console TTY (the B21/B22 line discipline).
#
# We use the well-known robxu9/bash-static build: GNU bash 5.2-015, statically
# linked against musl 1.2.3 (the exact upstream GNU bash sources, compiled by a
# public reproducible CI). Static-musl means it runs through tobyOS's proven
# static-Linux path (same loader surface as the bundled busybox) with no shared
# objects to version-match -- the milestone's new content is the *interactive
# bash read-eval loop over the TTY*, not the libc linkage (static + dynamic
# glibc/musl loading are already proven by B2/B5/B25/B26).
#
# The downloaded binary is left byte-for-byte pristine here; the Makefile brands
# EI_OSABI=Linux at initrd-assembly time (like the bundled busybox) so the Linux
# personality latches deterministically.
#
# Usage:   bash programs/realbash/build.sh
# Output:  programs/realbash/bash       (unmodified GNU bash 5.2, static musl)
# (gitignored; opt-in -- the Makefile stages it only when present)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/bash"

# Pinned release (GNU bash 5.2-015 + musl 1.2.3).
URL="https://github.com/robxu9/bash-static/releases/download/5.2.015-1.2.3-2/bash-linux-x86_64"

if [ -f "$OUT" ]; then
    echo "[build.sh] $OUT already present -- reusing"
else
    echo "[build.sh] fetching unmodified GNU bash 5.2 (static) ..."
    curl -fSL -o "$OUT" "$URL"
    chmod +x "$OUT"
fi

# Sanity: must be an x86-64 ELF.
sz=$(wc -c < "$OUT")
magic=$(head -c 4 "$OUT" | od -An -tx1 | tr -d ' ')
echo "[build.sh] staged GNU bash: $OUT ($sz bytes, magic=$magic)"
[ "$magic" = "7f454c46" ] || { echo "[build.sh] not an ELF -- download failed"; exit 1; }
echo "[build.sh] done."
