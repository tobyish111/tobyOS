#!/bin/bash
# lxsock: socket semantics that need a REAL PEER (no loopback exists in this
# stack, so the host over SLIRP hostfwd is the only honest one). The b14
# pattern: host 127.0.0.1:18083 -> guest :8081. Asserts BOTH halves:
#   guest (LXSOCKSRV bits): blocking accept survives >3s; pre-data
#     MSG_DONTWAIT recv is EAGAIN; half-close keeps rx alive; post-FIN send
#     is EPIPE.
#   host: reads PONG then a REAL EOF (the guest's FIN arrived on the wire),
#     and its post-EOF line still gets through.
#
# Assumes tobyOS.iso was built with EXTRA_CFLAGS+=-DLXSOCK_BOOT.
cd /c/CustomOS/tobyOS || exit 1
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
LOG="logs/lxsock.log"
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1
: > "$LOG"
"$QEMU" -cdrom tobyOS.iso \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough -boot d \
  -netdev user,id=net0,hostfwd=tcp::18083-:8081 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -smp 4 -m 1024 -cpu qemu64,+smep,+smap -serial "file:$LOG" \
  -no-reboot -display none &

for i in $(seq 1 120); do grep -aq 'lxsock\] listening' "$LOG" 2>/dev/null && break; sleep 1; done
if ! grep -aq 'lxsock\] listening' "$LOG" 2>/dev/null; then
  echo "SERVER NEVER LISTENED"; grep -aE '\[lxsock\]|LXSOCKSRV' "$LOG"
  taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; exit 1
fi

# THE POINT of this sleep: the old kernel's "blocking" accept returned EAGAIN
# after 3 s. Connecting at ~4.5 s separates "waits indefinitely" from "waited
# a bit". Do not shorten it below 4.
echo "== host: waiting 4.5s before connecting (the old kernel gave up at 3s)"
sleep 4.5

HOST_EOF=0; HOST_PONG=0
if exec 3<>/dev/tcp/127.0.0.1/18083 2>/dev/null; then
  # Say NOTHING until the guest's RDY token: the quiet window between our
  # connect and this token is when the guest runs its pre-data EAGAIN probe.
  RDY=$(timeout 10 head -c 4 <&3)
  if [ "$(printf %s "$RDY" | tr -d '\r\n')" = "RDY" ]; then
    printf 'PING' >&3
    RESP=$(timeout 8 head -c 5 <&3)          # "PONG\n"
    [ "$(printf %s "$RESP" | tr -d '\r\n')" = "PONG" ] && HOST_PONG=1
    # After PONG the guest sent FIN. EOF is an EMPTY read that returns
    # QUICKLY -- an empty read that took the whole timeout is a hang, not a
    # FIN, and v1 conflated the two.
    T0=$(date +%s)
    TAIL=$(timeout 8 head -c 1 <&3)
    DT=$(( $(date +%s) - T0 ))
    [ -z "$TAIL" ] && [ "$DT" -lt 5 ] && HOST_EOF=1
    # Half-close: our line must still reach the guest.
    printf 'AFTER' >&3 2>/dev/null
  else
    echo "host never got RDY (got: '$RDY')"
  fi
  exec 3<&- ; exec 3>&-
else
  echo "host connect FAILED"
fi
echo "== host observations: PONG=$HOST_PONG EOF-after-PONG=$HOST_EOF"

for i in $(seq 1 30); do grep -aq 'LXSOCKSRV. VERDICT' "$LOG" 2>/dev/null && break; sleep 1; done
sleep 1
echo "== guest output"
grep -aE '\[lxsock\]|LXSOCKSRV' "$LOG" | sed 's/^\[[0-9 ]*ms\] //'

RC=0
grep -aq 'LXSOCKSRV. VERDICT bits=15' "$LOG" || { echo "!! guest bits != 15"; RC=1; }
[ "$HOST_PONG" = 1 ] || { echo "!! host never got PONG"; RC=1; }
[ "$HOST_EOF" = 1 ]  || { echo "!! host never saw the FIN (no half-close on the wire)"; RC=1; }
FAULTS=$(grep -ac 'EXCEPTION\|user-mode fault\|PANIC' "$LOG")
[ "$FAULTS" != "0" ] && { echo "!! faults=$FAULTS"; RC=1; }
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1
echo
[ "$RC" = 0 ] && echo "RESULT: GREEN" || echo "RESULT: RED"
exit $RC
