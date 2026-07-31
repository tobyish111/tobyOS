#!/bin/bash
# Slice 73: is the ICU failure caused by OUR LIBRARIES, or merely by invoking
# chrome through an explicit loader?
#
# Running `ld.so ./chrome` makes /proc/self/exe point at ld.so, and chrome
# locates icudtl.dat relative to its own executable path -- so the explicit
# invocation alone can break ICU. Control for it by doing the same thing with
# UBUNTU's loader and system libraries:
#   ubuntu-ld also fails -> the loader invocation is the artifact; our libs are fine
#   only our-ld fails    -> the library set really is implicated
# This matters because tobyOS's chrome also readlink()s /proc/self/exe right
# before it dies.
set -u
CD=/mnt/c/CustomOS/tobyOS/programs/chromium/chrome-linux64
SYSROOT=/mnt/c/CustomOS/tobyOS/programs/chromium/sysroot
OUT=/mnt/c/CustomOS/tobyOS/logs/control
mkdir -p "$OUT"; cd "$CD" || exit 1

go() {   # go <label> <loader> [--library-path <p>]
    local label="$1"; shift
    rm -rf /tmp/cr_$label
    timeout 45 "$@" ./chrome --headless=new --no-sandbox --no-zygote \
        --disable-kill-after-bad-ipc --disable-dev-shm-usage \
        --disable-crash-reporter --disable-in-process-stack-traces \
        --no-first-run --enable-logging=stderr --v=1 \
        --user-data-dir=/tmp/cr_$label --window-size=800,600 \
        --mute-audio --disable-audio-output \
        --dump-dom about:blank >"$OUT/out_$label.txt" 2>"$OUT/err_$label.txt"
    printf '%-14s exit=%-5s %s\n' "$label" "$?" \
        "$(grep -aoiE 'invalid file descriptor to ICU data|check failed[^\"]{0,40}' \
            "$OUT/err_$label.txt" | head -1)"
}

echo "=== does an explicit loader alone break ICU? ==="
go ubuntu_ld  /lib64/ld-linux-x86-64.so.2
go our_ld     "$SYSROOT/ld-linux-x86-64.so.2" --library-path "$CD:$SYSROOT"
echo
echo "=== and with chrome told explicitly where its ICU data is ==="
rm -rf /tmp/cr_icufix
timeout 45 "$SYSROOT/ld-linux-x86-64.so.2" --library-path "$CD:$SYSROOT" \
    ./chrome --headless=new --no-sandbox --no-zygote --no-first-run \
    --enable-logging=stderr --v=1 --user-data-dir=/tmp/cr_icufix \
    --disable-crash-reporter --disable-in-process-stack-traces \
    --icu-data-file="$CD/icudtl.dat" \
    --dump-dom about:blank >"$OUT/out_icufix.txt" 2>"$OUT/err_icufix.txt"
printf '%-14s exit=%-5s %s\n' icu_data_flag "$?" \
    "$(grep -aoiE 'invalid file descriptor to ICU data|check failed[^\"]{0,40}' \
        "$OUT/err_icufix.txt" | head -1)"
echo "(exit 124 = ran the full 45s = healthy)"
