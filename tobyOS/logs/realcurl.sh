#!/bin/bash
# REALCURL: run UNMODIFIED upstream static curl 8.20.0 (libcurl/musl) fetching
# http://example.com/ over e1000 + SLIRP. SLIRP gives DHCP (10.0.2.15) + a DNS
# proxy (10.0.2.3) reaching the real internet via the host, so this needs host
# internet. PASS iff curl exits 0 (only when the HTTP transfer completed).
# MEMORY: -m 512 was too small and produced a LIMINE panic ("High memory
# allocator: Out of memory") while loading an ~826 MB initrd -- i.e. a
# BOOTLOADER failure that never reaches the kernel, reported here as a
# silent absence of any REALCURL line. validate.sh already carries this
# exact fix and its comment; this script never got it. 5120 matches every
# other gate.
# Assumes tobyOS.iso built with EXTRA_CFLAGS+=-DREALCURL_BOOT and the opt-in
# static curl staged at programs/realcurl/curl (programs/realcurl/build.sh).
set -e
cd /c/CustomOS/tobyOS || exit 1

echo "=== stage unmodified static curl ==="
bash programs/realcurl/build.sh

echo "=== build ISO (REALCURL_BOOT) ==="
touch src/kernel.c
make iso EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DREALCURL_BOOT" >logs/realcurl.build.log 2>&1
[ -f tobyOS.iso ] || { echo "BUILD FAIL"; tail -40 logs/realcurl.build.log; exit 1; }
echo "built."

QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
LOG="logs/realcurl.log"
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1 || true; sleep 1
: > "$LOG"
"$QEMU" -cdrom tobyOS.iso \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough -boot d \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -object filter-dump,id=fd0,netdev=net0,file=logs/realcurl.pcap \
  -smp 4 -m 5120 -cpu qemu64,+smep,+smap -serial "file:$LOG" \
  -no-reboot -display none &
for i in $(seq 1 240); do grep -aq 'REALCURL\] VERDICT' "$LOG" 2>/dev/null && break; sleep 1; done
sleep 1
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1 || true

echo "=== REALCURL: real unmodified curl HTTP GET over e1000+SLIRP ==="
grep -aE 'REALCURL|lease applied|\[net\] up|\[dns\]|unhandled syscall' "$LOG" | head -40
echo "=== faults / unhandled (filtered; empty=clean) ==="
grep -aiE 'KERNEL PANIC|#GP|#XF|EXCEPTION [0-9]|rip=0x0|unhandled syscall|page fault' "$LOG" \
  | grep -viE '\[pe\] +bind|SetUnhandledException|reset_reg|milestone' | head
echo "(end)"
