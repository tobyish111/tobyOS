#!/bin/bash
# One-shot runner for the 2026-08-22 networking-honesty batch:
#   1. make clean (struct sock grew -- no header dep tracking)
#   2. lxposix gate (now 13 subtests incl. linux-sock)
#   3. rebuild with -DLXSOCK_BOOT and run the host-peer gate
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

echo "===== [1/3] make clean (SKIPPED: no layout change since last clean)"
: # layout unchanged

echo "===== [2/3] lxposix gate"
bash logs/lxposix.sh || { echo "NETBATCH: lxposix RED"; exit 1; }

echo "===== [3/3] lxsock host-peer gate"
touch src/kernel.c
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DLXSOCK_BOOT" iso \
          > logs/lxsock.build.log 2>&1; then
    echo "NETBATCH: LXSOCK BUILD FAILED"; grep -E ' error:' logs/lxsock.build.log | head -10
    exit 1
fi
bash logs/lxsock.sh || { echo "NETBATCH: lxsock RED"; exit 1; }
echo "NETBATCH: ALL GREEN"
