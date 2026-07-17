#!/bin/sh
# Edge-as-oracle comparison: build tobyOS with the URL baked in, boot +
# screendump, render the same URL in headless Edge, emit a side-by-side
# composite + grid-diff score.
#
# usage: tools/compare/compare.sh <url> [slug] [outdir]
#   outdir default: tools/compare/out (gitignored). MSYS2 bash only.
# env: COMPARE_WAIT (settle seconds, default 70), COMPARE_SKIP_BUILD=1
#      (reuse the current ISO), COMPARE_SKIP_TOBY=1 (reuse toby_<slug>.png).
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
URL="$1"
[ -n "$URL" ] || { echo "usage: compare.sh <url> [slug] [outdir]"; exit 1; }
SLUG="${2:-$(echo "$URL" | sed 's|https\?://||; s|[^a-zA-Z0-9]|_|g' | cut -c1-40)}"
OUT="${3:-$REPO/tools/compare/out}"
mkdir -p "$OUT"
OUT_W="$(cygpath -w "$OUT")"

PY="/c/Users/tdude/AppData/Local/Programs/Python/Python311/python"
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

if [ -z "$COMPARE_SKIP_BUILD" ]; then
    echo "== building ISO with NAV_URL=$URL"
    cd "$REPO"
    touch programs/user_gui_browser/main.c src/kernel.c
    make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
         BROWSER_EXTRA="-DNAV_URL=\\\"$URL\\\"" \
         EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTKAPP_BOOT -DTKAPP_BROWSER" \
         iso > "$OUT/build_$SLUG.log" 2>&1 \
        || { tail -20 "$OUT/build_$SLUG.log"; exit 1; }
fi

EXTRA=""
[ -n "$COMPARE_SKIP_TOBY" ] && EXTRA="--skip-toby"
"$PY" "$REPO/tools/compare/compare.py" --url "$URL" --slug "$SLUG" \
      --out "$OUT_W" --wait "${COMPARE_WAIT:-70}" $EXTRA
