#!/bin/bash
# Slice 73: bisect the ENVIRONMENT, not the flags. Slice 72 proved the binary
# + flags are healthy on real Linux, so what differs is the environment
# chromewin hands chrome:
#     HOME=/data TMPDIR=/data PATH=/bin LANG=C
#     LD_LIBRARY_PATH=/opt/chrome:/opt/chrome/sysroot   <- OUR bookworm sysroot
#     LD_PRELOAD=/opt/chrome/libvulkan.so.1:/opt/chrome/libvk_swiftshader.so
#     VK_ICD_FILENAMES=/opt/chrome/vk_swiftshader_icd.json
#     DISPLAY=:0            <- on tobyOS this is a HANDSHAKE-ONLY fake X server
#     FONTCONFIG_FILE=/etc/fonts/fonts.conf
# Each variant adds one property. First one that CHECKs names the culprit --
# and reproduces it on Linux, off the 7-minute guest-boot loop entirely.
set -u
CD=/mnt/c/CustomOS/tobyOS/programs/chromium/chrome-linux64
SYSROOT=/mnt/c/CustomOS/tobyOS/programs/chromium/sysroot
OUT=/mnt/c/CustomOS/tobyOS/logs/control
mkdir -p "$OUT"; cd "$CD" || exit 1

try() {                        # try <label> [VAR=VAL ...]
    local label="$1"; shift
    rm -rf /tmp/cr_$label
    timeout 45 env "$@" ./chrome --headless=new --no-sandbox --no-zygote \
        --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
        --in-process-gpu --disable-kill-after-bad-ipc --disable-dev-shm-usage \
        --disable-crash-reporter --disable-in-process-stack-traces \
        --no-first-run --enable-logging=stderr --v=1 \
        --user-data-dir=/tmp/cr_$label --window-size=800,600 \
        --mute-audio --disable-audio-output \
        --dump-dom about:blank >"$OUT/out_$label.txt" 2>"$OUT/err_$label.txt"
    local rc=$?
    local bad
    bad=$(grep -aicE 'check failed|FATAL|received signal|Trace/breakpoint' "$OUT/err_$label.txt")
    printf '%-12s exit=%-4s stderr=%-5s CHECK/FATAL=%s\n' \
        "$label" "$rc" "$(wc -l <"$OUT/err_$label.txt")" "$bad"
    [ "$bad" != "0" ] && grep -aiE 'check failed|FATAL|received signal' \
        "$OUT/err_$label.txt" | head -2 | cut -c1-150 | sed 's/^/             /'
    return 0
}

echo "=== environment bisection (exit 124 = ran the full 45s = healthy) ==="
try baseline
try preload LD_PRELOAD="$CD/libvulkan.so.1:$CD/libvk_swiftshader.so"
try sysroot LD_LIBRARY_PATH="$CD:$SYSROOT"
try display DISPLAY=:0
mkdir -p /tmp/tobyhome
try minenv HOME=/tmp/tobyhome TMPDIR=/tmp/tobyhome LANG=C
try fulltoby LD_LIBRARY_PATH="$CD:$SYSROOT" \
             LD_PRELOAD="$CD/libvulkan.so.1:$CD/libvk_swiftshader.so" \
             VK_ICD_FILENAMES="$CD/vk_swiftshader_icd.json" \
             DISPLAY=:0 LANG=C HOME=/tmp/tobyhome TMPDIR=/tmp/tobyhome
