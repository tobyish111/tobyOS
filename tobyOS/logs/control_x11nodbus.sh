#!/bin/bash
# Does chrome still reach MapWindow with NO reachable dbus (session+system
# pointed at a nonexistent socket)? tobyOS has no dbus at all; if this hangs
# on a real X server, the headed wall is dbus, not X.
set -u
CD=/mnt/c/CustomOS/tobyOS/programs/chromium/chrome-linux64
OUT=/mnt/c/CustomOS/tobyOS/logs/control
pkill -f "Xvfb :77" 2>/dev/null; pkill -f xtrace 2>/dev/null; sleep 0.5
Xvfb :77 -listen tcp -screen 0 800x600x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1
xtrace -n -k -D localhost:79 -d localhost:77 -o "$OUT/x11nodbus.txt" >/dev/null 2>&1 &
XT=$!
sleep 1
cd "$CD"
rm -rf /tmp/crnd && mkdir -p /tmp/crnd
timeout 40 env DISPLAY=localhost:79 HOME=/tmp/crnd \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
    DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent \
    ./chrome \
    --ozone-platform=x11 \
    --enable-features=UseOzonePlatform,NetworkServiceInProcess \
    --no-sandbox --no-zygote --single-process --remote-debugging-pipe \
    --in-process-gpu --disable-gpu --disable-vulkan --use-gl=disabled \
    --disable-kill-after-bad-ipc --disable-dev-shm-usage \
    --disable-crash-reporter --disable-in-process-stack-traces \
    --no-first-run --no-default-browser-check --disable-component-update \
    --enable-logging=stderr --v=1 \
    --user-data-dir=/tmp/crnd/ud \
    --disable-extensions --disable-background-networking \
    --disable-component-extensions-with-background-pages \
    --disable-features=OptimizationHints,MediaRouter \
    https://example.com \
    3</dev/null 4>/dev/null > "$OUT/nodbuschrome.log" 2>&1
kill $XT $XVFB 2>/dev/null
echo "=== mapped? ==="
grep -c "MapWindow" "$OUT/x11nodbus.txt"
grep -nE "width=780" "$OUT/x11nodbus.txt" | head -2
