#!/bin/bash
# Tier 2.5: ground truth for the headed Ozone X11 bring-up. Run the SAME
# chrome binary with the SAME flag set the guest uses, against a real X
# server (Xvfb), with the wire protocol logged by xtrace. The resulting
# request/reply/event sequence is the spec our fake xserver.c must satisfy
# for chrome to reach MapWindow + ShmPutImage.
#
# Invocation form matters: `timeout N env VAR=... ./chrome` -- env after
# timeout, or the vars land on timeout itself (cost a batch once).
set -u
CD=/mnt/c/CustomOS/tobyOS/programs/chromium/chrome-linux64
OUT=/mnt/c/CustomOS/tobyOS/logs/control
mkdir -p "$OUT"

# WSLg mounts /tmp/.X11-unix READ-ONLY, so Unix-socket displays cannot
# bind. Everything runs over TCP displays instead.
D_REAL=localhost:77   # Xvfb -listen tcp
D_FAKE=localhost:78   # xtrace proxy in front of it

pkill -f "Xvfb :77" 2>/dev/null; pkill -f "xtrace" 2>/dev/null; sleep 0.5

Xvfb :77 -listen tcp -screen 0 800x600x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1

# xtrace: -n = don't copy X auth (Xvfb runs open); -k = keep serving.
xtrace -n -k -D $D_FAKE -d $D_REAL -o "$OUT/x11wire.txt" >/dev/null 2>&1 &
XT_PID=$!
sleep 1

cd "$CD"
rm -rf /tmp/crx11 && mkdir -p /tmp/crx11
timeout 45 env DISPLAY=$D_FAKE HOME=/tmp/crx11 \
    ./chrome \
    --ozone-platform=x11 \
    --enable-features=UseOzonePlatform,NetworkServiceInProcess \
    --no-sandbox --no-zygote --single-process \
    --remote-debugging-pipe \
    --in-process-gpu --disable-gpu --disable-vulkan --use-gl=disabled \
    --disable-kill-after-bad-ipc --disable-dev-shm-usage \
    --disable-crash-reporter --disable-in-process-stack-traces \
    --no-first-run --no-default-browser-check --disable-component-update \
    --enable-logging=stderr --v=1 \
    --user-data-dir=/tmp/crx11/ud \
    --disable-extensions --disable-background-networking \
    --disable-component-extensions-with-background-pages \
    --disable-features=OptimizationHints,MediaRouter \
    https://example.com \
    3</dev/null 4>/dev/null \
    > "$OUT/x11chrome.log" 2>&1

kill $XT_PID $XVFB_PID 2>/dev/null
echo "=== wire log size ==="
wc -l "$OUT/x11wire.txt"
echo "=== request opcode histogram ==="
grep -oE "Request\([0-9]+\): [A-Za-z]+" "$OUT/x11wire.txt" | sort | uniq -c | sort -rn | head -40
echo "=== extension queries ==="
grep -E "QueryExtension" "$OUT/x11wire.txt" | head -30
echo "=== MapWindow + Shm ==="
grep -nE "MapWindow|ShmAttach|ShmPutImage|CreateWindow" "$OUT/x11wire.txt" | head -20
