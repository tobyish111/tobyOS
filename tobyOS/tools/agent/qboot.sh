#!/bin/bash
# qboot.sh LOG [ISO [QEMU-ARGS...]] -- (re)boot an ISO (default tobyOS.iso) in QEMU, -smp 4, with
# SSH on host port 2222 and the kernel shell (7777) on 17777; wait for the SSH banner.
# LOG must be a RELATIVE path (QEMU is a Windows program; an MSYS /c/... path makes
# it write nothing and never boot). QEMU-ARGS are added as given (a second disk:
# -drive file=C:/t/b36/x.img,format=raw,if=virtio).
cd "$(dirname "$0")/../.." || exit 1
LOG=$1; ISO=${2:-tobyOS.iso}; shift; shift
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1
rm -f "$LOG"
("/c/Program Files/qemu/qemu-system-x86_64.exe" -cdrom "$ISO" -drive file=disk.img,format=raw,if=ide,index=0,media=disk -boot d -smp 4 -m 5120 -netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::17777-:7777 -device e1000,netdev=net0,mac=52:54:00:12:34:56 -serial file:"$LOG" -no-reboot -display none "$@" > /dev/null 2>&1 &)
python - <<'PY'
import socket, time
for i in range(80):
    try:
        s = socket.create_connection(("127.0.0.1", 2222), timeout=3); s.settimeout(5); b = s.recv(64); s.close()
        if b.startswith(b"SSH-"): print("qemu up"); break
    except Exception: pass
    time.sleep(3)
else:
    print("qemu NOT up")
PY
