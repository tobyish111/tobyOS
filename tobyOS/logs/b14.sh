#!/bin/bash
# B14: networking capstone -- an event-driven Linux TCP echo server
# (/bin/linux-netserver) built on the Linux socket family + epoll, with
# TCP-socket readiness wired into the kernel poll/epoll predicate. A REAL
# host client connects over SLIRP hostfwd (host 127.0.0.1:18082 -> guest
# 10.0.2.15:8080) and verifies the echo. +smap.
#
# Assumes tobyOS.iso was built with EXTRA_CFLAGS+=-DLXNETSRV_BOOT.
cd /c/CustomOS/tobyOS || exit 1
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
LOG="logs/b14.log"; PORT=4495
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1
: > "$LOG"
"$QEMU" -cdrom tobyOS.iso \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough -boot d \
  -netdev user,id=net0,hostfwd=tcp::18082-:8080 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -smp 4 -m 512 -cpu qemu64,+smep,+smap -serial "file:$LOG" \
  -qmp tcp:127.0.0.1:$PORT,server,nowait -no-reboot -display none &

# Wait for the server to announce it is listening.
for i in $(seq 1 120); do grep -aq 'b14\] listening' "$LOG" 2>/dev/null && break; sleep 1; done
if ! grep -aq 'b14\] listening' "$LOG" 2>/dev/null; then
  echo "SERVER NEVER LISTENED -- dumping b14 lines:"; grep -aE '\[b14\]|LXNETSRV' "$LOG"
  taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; exit 1
fi
echo "=== host client connecting to 127.0.0.1:18082 ==="
sleep 1
RESP=""
if exec 3<>/dev/tcp/127.0.0.1/18082 2>/dev/null; then
  printf 'TOBYNET hello from the host\n' >&3
  RESP=$(timeout 6 head -c 64 <&3)
  exec 3<&- ; exec 3>&-
  echo "host received echo from tobyOS server: \"$(echo "$RESP" | tr -d '\r' | head -1)\""
else
  echo "host /dev/tcp connect FAILED"
fi
# Wait for the server verdict.
for i in $(seq 1 60); do grep -aq 'LXNETSRV\] VERDICT' "$LOG" 2>/dev/null && break; sleep 1; done
sleep 1
echo "=== [b14] server output ==="
grep -aE '\[b14\]|LXNETSRV|net_up=' "$LOG"
echo "=== faults (filtered; empty=clean) ==="
grep -aiE 'KERNEL PANIC|#PF|#GP|#XF|page fault|EXCEPTION|rip=0x0|unhandled syscall' "$LOG" | grep -viE '\[pe\] +bind|SetUnhandledException|reset_reg' | head
echo "(end)"
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1
