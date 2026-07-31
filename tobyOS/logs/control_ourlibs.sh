#!/bin/bash
# Slice 73 (decisive split): run chrome with OUR loader + OUR libraries only,
# on a known-good kernel.
#
# The plain LD_LIBRARY_PATH test was contaminated: pointing Ubuntu's glibc-2.43
# binaries at our bookworm-era sysroot shadows the system libc/libm and
# segfaults for reasons that do not exist on tobyOS (where our in-tree glibc is
# the ONLY glibc). Invoking OUR ld.so with --library-path removes that mixing:
# the library world is then exactly tobyOS's, but the kernel underneath is
# Linux. So:
#   fails here  -> the LIBRARY SET is the problem; tobyOS's kernel is exonerated
#   runs here   -> libraries are fine; the difference IS our kernel/syscalls
set -u
CD=/mnt/c/CustomOS/tobyOS/programs/chromium/chrome-linux64
SYSROOT=/mnt/c/CustomOS/tobyOS/programs/chromium/sysroot
OUT=/mnt/c/CustomOS/tobyOS/logs/control
mkdir -p "$OUT"; cd "$CD" || exit 1

LD="$SYSROOT/ld-linux-x86-64.so.2"
echo "=== our loader: $(ls -la "$LD" | awk '{print $5}') bytes"
"$LD" --version 2>&1 | head -1

rm -rf /tmp/cr_ourlibs
timeout 60 "$LD" --library-path "$CD:$SYSROOT" ./chrome \
    --headless=new --no-sandbox --no-zygote \
    --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
    --in-process-gpu --disable-kill-after-bad-ipc --disable-dev-shm-usage \
    --disable-crash-reporter --disable-in-process-stack-traces --no-first-run \
    --enable-logging=stderr --v=1 --user-data-dir=/tmp/cr_ourlibs \
    --window-size=800,600 --mute-audio --disable-audio-output \
    --dump-dom about:blank >"$OUT/out_ourlibs.txt" 2>"$OUT/err_ourlibs.txt"
rc=$?
echo "exit=$rc  (124=ran the full 60s=healthy, 139=SIGSEGV, 133=SIGTRAP/CHECK)"
echo "stderr lines: $(wc -l <"$OUT/err_ourlibs.txt")"
echo "=== first lines:"; head -4 "$OUT/err_ourlibs.txt" | cut -c1-170
echo "=== CHECK/FATAL/signal:"
grep -aiE 'check failed|FATAL|received signal|Trace/break' "$OUT/err_ourlibs.txt" | head -3 | cut -c1-170
echo "(nothing above + exit 124 = our library set is FINE on a real kernel)"
