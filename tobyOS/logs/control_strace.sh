#!/bin/bash
# Slice 74: what does a HEALTHY chrome do at the exact point tobyOS's chrome
# dies? tobyOS's [lx-recent] tail for the browser main thread is:
#   getrandom, mkdir, stat, gettid, prctl, rt_sigprocmask, sendmsg,
#   rt_sigprocmask, rt_sigaction, getpid, gettid, exit_group(191)
# Strace the same binary on Linux (main thread only -- that is the thread
# that dies), find that region, and print what comes after the sendmsg.
# Also answers WHAT the sendmsg targets, which the tobyOS ring cannot show.
set -u
CD=/mnt/c/CustomOS/tobyOS/programs/chromium/chrome-linux64
OUT=/mnt/c/CustomOS/tobyOS/logs/control
mkdir -p "$OUT"; cd "$CD" || exit 1

command -v strace >/dev/null || {
    export DEBIAN_FRONTEND=noninteractive
    apt-get install -y -qq strace >/dev/null 2>&1
}
echo "strace: $(command -v strace || echo MISSING)"

rm -rf /tmp/cr_strace
timeout 45 strace -o "$OUT/strace.txt" -s 120 \
    ./chrome --headless=new --no-sandbox --no-zygote \
    --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
    --in-process-gpu --disable-kill-after-bad-ipc --disable-dev-shm-usage \
    --disable-crash-reporter --disable-in-process-stack-traces --no-first-run \
    --enable-logging=stderr --user-data-dir=/tmp/cr_strace \
    --window-size=800,600 --mute-audio --disable-audio-output \
    --dump-dom about:blank >/dev/null 2>"$OUT/err_strace.txt"
echo "traced $(wc -l <"$OUT/strace.txt") syscalls (main thread)"

echo
echo "=== the mkdir/prctl/sendmsg region (tobyOS dies just after this) ==="
grep -n -E '^(mkdir|prctl|sendmsg|getrandom|rt_sigaction)' "$OUT/strace.txt" | head -12
echo
N=$(grep -n '^sendmsg' "$OUT/strace.txt" | head -1 | cut -d: -f1)
if [ -n "${N:-}" ]; then
    echo "=== first sendmsg is line $N; the 18 syscalls AFTER it: ==="
    sed -n "$((N)),$((N+18))p" "$OUT/strace.txt" | cut -c1-150
else
    echo "(no sendmsg on the main thread -- compare the region above instead)"
fi
echo
echo "=== what socket does chrome connect() to early on? ==="
grep -n -E '^(socket|connect)' "$OUT/strace.txt" | head -8 | cut -c1-150
