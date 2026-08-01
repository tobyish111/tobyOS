#!/bin/bash
# Tier 2.5: does chrome still reach MapWindow when the X server offers NO
# extensions (xtrace --denyextensions), i.e. a surface even sparser than our
# fake server's? If it hangs like tobyOS -> an extension we don't provide is
# load-bearing; bisect. If it maps -> our extension surface is sufficient and
# the divergence is in reply/event BYTES, not coverage.
set -u
CD=/mnt/c/CustomOS/tobyOS/programs/chromium/chrome-linux64
OUT=/mnt/c/CustomOS/tobyOS/logs/control
mkdir -p "$OUT"

pkill -f "Xvfb :77" 2>/dev/null; pkill -f xtrace 2>/dev/null; sleep 0.5
Xvfb :77 -listen tcp -screen 0 800x600x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1
xtrace -n -k -e -D localhost:79 -d localhost:77 -o "$OUT/x11deny.txt" >/dev/null 2>&1 &
XT=$!
sleep 1

cd "$CD"
rm -rf /tmp/crdeny && mkdir -p /tmp/crdeny
timeout 45 env DISPLAY=localhost:79 HOME=/tmp/crdeny \
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
    --user-data-dir=/tmp/crdeny/ud \
    --disable-extensions --disable-background-networking \
    --disable-component-extensions-with-background-pages \
    --disable-features=OptimizationHints,MediaRouter \
    https://example.com \
    3</dev/null 4>/dev/null \
    > "$OUT/denychrome.log" 2>&1

kill $XT $XVFB 2>/dev/null
echo "=== did it map? ==="
grep -cE "MapWindow" "$OUT/x11deny.txt"
grep -nE "width=780|MapWindow" "$OUT/x11deny.txt" | head -5
echo "=== chrome fatal? ==="
grep -iE "FATAL|hang|failed" "$OUT/denychrome.log" | head -5
