#!/bin/bash
# Slice 72 CONTROL (this project's prime directive): run the SAME full chrome
# binary with the SAME flags tobyOS uses, on a known-good Linux (WSL2 Ubuntu),
# and see whether it also dies at a CHECK.
#   runs there + dies on tobyOS -> the difference is ours; diff the environment
#   dies there too                -> flags/environment are wrong, tobyOS exonerated
# Run: wsl -d Ubuntu -u root -- bash /mnt/c/CustomOS/tobyOS/logs/control_fullchrome.sh
set -u
CD=/mnt/c/CustomOS/tobyOS/programs/chromium/chrome-linux64
cd "$CD" || exit 1
OUT=/mnt/c/CustomOS/tobyOS/logs/control
mkdir -p "$OUT"

run() {                       # run <label> <extra-flags...>
    local label="$1"; shift
    rm -rf /tmp/cr_$label
    timeout 90 ./chrome \
        --headless=new --no-sandbox --no-zygote \
        --disable-kill-after-bad-ipc --disable-dev-shm-usage \
        --disable-crash-reporter --disable-in-process-stack-traces \
        --no-first-run --enable-logging=stderr --v=1 \
        --user-data-dir=/tmp/cr_$label --window-size=800,600 \
        --mute-audio --disable-audio-output \
        "$@" --dump-dom about:blank >$OUT/out_$label.txt 2>$OUT/err_$label.txt
    local rc=$?
    printf '%-14s exit=%-4s dom=%s bytes  stderr=%s lines\n' \
        "$label" "$rc" "$(wc -c <$OUT/out_$label.txt)" "$(wc -l <$OUT/err_$label.txt)"
    grep -aiE 'check failed|fatal|received signal|ERROR:' $OUT/err_$label.txt \
        | head -3 | sed 's/^/                 /'
}

echo "=== chrome $(./chrome --version 2>/dev/null) on $(uname -sr) ==="
# 1. tobyOS's exact GL flag set (the one chromewin ships).
run tobyflags --use-gl=angle --use-angle=swiftshader \
              --enable-unsafe-swiftshader --in-process-gpu
# 2. The variant tested in slice 71 (also CHECKed on tobyOS).
run disablegpu --disable-gpu
# 3. Bare minimum -- isolates whether ANY of the GL flags matter here.
run bare
