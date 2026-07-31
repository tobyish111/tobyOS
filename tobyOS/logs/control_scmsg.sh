#!/bin/bash
# Slice 82: build + run the AF_UNIX socketpair sendmsg/recvmsg ABI test on a
# REAL Linux kernel first. If it does not PASS here, the test is wrong -- not
# tobyOS. (Exit codes: 3=PASS, 10..15 name the failing step; see main.c.)
set -u
SRC=/mnt/c/CustomOS/tobyOS/programs/linux-scmsg/main.c
OUT=/mnt/c/CustomOS/tobyOS/logs/control
mkdir -p "$OUT"
BIN=/tmp/linux-scmsg

gcc -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-pic -fno-pie -O2 -Wl,-e,_start -o "$BIN" "$SRC" 2>"$OUT/scmsg_build.txt" || {
    echo "BUILD FAILED:"; cat "$OUT/scmsg_build.txt"; exit 1; }
echo "built $(stat -c%s "$BIN") bytes"
"$BIN"; rc=$?
echo "exit=$rc  (3=PASS; 10=socketpair 11=MSG_NOSIGNAL-send 12=recv 13=size 14=corrupt 15=send)"
