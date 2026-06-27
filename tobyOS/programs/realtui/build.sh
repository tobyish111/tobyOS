#!/bin/bash
# realtui: stage a REAL, unmodified, off-the-shelf full-screen TUI application
# so tobyOS can run it INTERACTIVELY over the kernel console -- proving the new
# VT100/ANSI terminal emulator in src/console.c renders a genuine ncurses UI.
#
# We use the public Alpine Linux build of GNU nano (the canonical curses text
# editor): a dynamic musl program (nano -> libncursesw.so.6 -> libc.musl) that
# runs through the SAME musl loader busybox-dyn / CPython / TinyCC use, and --
# because it links libncursesw -- also exercises the multi-DSO musl path.
# ncurses needs a terminfo description for $TERM, so we also stage Alpine's
# ncurses-terminfo-base (we drive nano with TERM=linux).
#
# Versions are resolved live from the edge/main APKINDEX so this keeps working
# as Alpine rolls forward (no hard-coded version to rot).
#
# Usage:   bash programs/realtui/build.sh
# Outputs: programs/realtui/nano               (UNMODIFIED GNU nano, dyn musl)
#          programs/realtui/libncursesw.so.6   (its curses backend)
#          programs/realtui/terminfo/           (terminfo db, incl. l/linux)
# (gitignored; opt-in -- the Makefile stages them only when present)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
NANOBIN="$HERE/nano"
NCURSES="$HERE/libncursesw.so.6"
TINFO="$HERE/terminfo"

BASE="https://dl-cdn.alpinelinux.org/alpine/edge/main/x86_64"
EXDIR="$HERE/extract"

if [ -f "$NANOBIN" ] && [ -f "$NCURSES" ] && [ -d "$TINFO" ]; then
    echo "[build.sh] nano + libncursesw + terminfo already present -- reusing"
else
    rm -rf "$EXDIR"; mkdir -p "$EXDIR"

    echo "[build.sh] fetching edge/main APKINDEX to resolve package versions ..."
    curl -fSL -o "$EXDIR/APKINDEX.tar.gz" "$BASE/APKINDEX.tar.gz"
    tar -C "$EXDIR" -xzf "$EXDIR/APKINDEX.tar.gz" APKINDEX

    # Resolve "P:<pkg>" -> "V:<ver>" from the index (blank-line-delimited records).
    pkgver() {
        awk -v pkg="$1" 'BEGIN{RS="";FS="\n"}
            { name=""; ver="";
              for (i=1;i<=NF;i++){ if($i ~ /^P:/) name=substr($i,3);
                                   if($i ~ /^V:/) ver=substr($i,3); }
              if (name==pkg){ print ver; exit } }' "$EXDIR/APKINDEX"
    }

    for pkg in nano libncursesw ncurses-terminfo-base; do
        ver="$(pkgver "$pkg")"
        [ -n "$ver" ] || { echo "[build.sh] could not resolve $pkg version"; exit 1; }
        apk="$EXDIR/${pkg}-${ver}.apk"
        echo "[build.sh] fetching ${pkg}-${ver}.apk ..."
        curl -fSL -o "$apk" "$BASE/${pkg}-${ver}.apk"
        # An .apk is a gzip tar (with .SIGN/.PKGINFO members); tar extracts the
        # real files (usr/...) and ignores the metadata members.
        tar -C "$EXDIR" -xzf "$apk" 2>/dev/null || true
    done

    [ -f "$EXDIR/usr/bin/nano" ] || { echo "[build.sh] missing usr/bin/nano"; exit 1; }
    cp "$EXDIR/usr/bin/nano" "$NANOBIN"
    chmod +x "$NANOBIN"

    # libncursesw.so.6 is usually a symlink to libncursesw.so.6.x -- copy the
    # real target out under the SONAME the binary asks for.
    real="$(ls "$EXDIR"/usr/lib/libncursesw.so.6* 2>/dev/null | while read f; do [ -f "$f" ] && [ ! -L "$f" ] && echo "$f"; done | head -1)"
    [ -n "$real" ] || { echo "[build.sh] missing libncursesw.so.6"; exit 1; }
    cp "$real" "$NCURSES"

    # terminfo lands in /usr/share/terminfo or /etc/terminfo depending on build.
    rm -rf "$TINFO"; mkdir -p "$TINFO"
    if [ -d "$EXDIR/usr/share/terminfo" ]; then
        cp -r "$EXDIR/usr/share/terminfo/." "$TINFO/"
    fi
    if [ -d "$EXDIR/etc/terminfo" ]; then
        cp -r "$EXDIR/etc/terminfo/." "$TINFO/"
    fi
    [ -f "$TINFO/l/linux" ] || [ -f "$TINFO/x/xterm" ] || \
        { echo "[build.sh] no linux/xterm terminfo entry found"; exit 1; }

    rm -rf "$EXDIR"
fi

szbin=$(wc -c < "$NANOBIN")
magic=$(head -c 4 "$NANOBIN" | od -An -tx1 | tr -d ' ')
echo "[build.sh] staged nano ($szbin bytes, magic=$magic)"
echo "[build.sh] DT_NEEDED (inspect for unstaged deps):"
( command -v readelf >/dev/null && readelf -d "$NANOBIN" | grep -i needed ) || true
echo "[build.sh] terminfo entries staged:"
ls "$TINFO" 2>/dev/null | tr '\n' ' '; echo
[ "$magic" = "7f454c46" ] || { echo "[build.sh] nano not an ELF -- download failed"; exit 1; }
echo "[build.sh] done."
