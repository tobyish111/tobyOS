#!/bin/bash
# Reference vlog with the same --vmodule set the guest now uses, so the two
# streams diff line-for-line. Same flags, Xvfb, no xtrace (not needed).
set -u
CD=/mnt/c/CustomOS/tobyOS/programs/chromium/chrome-linux64
OUT=/mnt/c/CustomOS/tobyOS/logs/control
pkill -f "Xvfb :77" 2>/dev/null; sleep 0.5
Xvfb :77 -listen tcp -screen 0 800x600x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1
cd "$CD"
rm -rf /tmp/crvm && mkdir -p /tmp/crvm
timeout 40 env DISPLAY=localhost:77 HOME=/tmp/crvm \
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
    "--vmodule=*/ui/gfx/x/*=3,*/ui/base/x/*=2,*/ui/ozone/*=2,*/ui/views/widget/*=2,*/ui/aura/*=1,*/chrome/browser/ui/views/frame/*=2" \
    --user-data-dir=/tmp/crvm/ud \
    --disable-extensions --disable-background-networking \
    --disable-component-extensions-with-background-pages \
    --disable-features=OptimizationHints,MediaRouter \
    https://example.com \
    3</dev/null 4>/dev/null > "$OUT/vmodchrome.log" 2>&1
kill $XVFB 2>/dev/null
grep -cE "VERBOSE" "$OUT/vmodchrome.log"
