#!/bin/bash
# Does an SSH session get /bin/tsh? Proven by running things the old
# 14-builtin term.c interpreter provably could not: pipelines, redirection,
# variables, and a real exit status.
#
# Harness rules learned the hard way:
#  - a leftover QEMU holds port 2222 AND the serial log, and the next run then
#    tests nothing while looking like it ran. Refuse to start if either is busy.
#  - `taskkill //F` is a NO-OP under MSYS_NO_PATHCONV=1; it must be /F.
#  - never grep for "" or "." as the expected output: both match a connection
#    error, which turned two failures green on an earlier run.
cd /c/CustomOS/tobyOS || exit 1
LOG=logs/sshtest.log
PORT=2222

MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
sleep 2
if [ "$(MSYS_NO_PATHCONV=1 netstat -ano 2>/dev/null | grep -c ":$PORT")" != "0" ]; then
    echo "ABORT: port $PORT still in use -- a previous VM is alive"; exit 1
fi
rm -f "$LOG" || { echo "ABORT: cannot remove $LOG (VM still holding it)"; exit 1; }

"/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
  -boot d -smp 4 -m 5120 -cpu max \
  -netdev user,id=net0,hostfwd=tcp::$PORT-:22 \
  -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -serial file:"$LOG" -no-reboot -display none 2> logs/sshtest.qemu.err &

for i in $(seq 1 60); do
  grep -aq "ssh\] listening" "$LOG" 2>/dev/null && break
  sleep 2
done
if grep -q "Could not set up host forwarding" logs/sshtest.qemu.err 2>/dev/null; then
    echo "ABORT: hostfwd failed -- port still taken"; exit 1
fi
if ! grep -aq "ssh\] listening" "$LOG" 2>/dev/null; then
  echo "ABORT: sshd never came up"; tail -5 "$LOG" 2>/dev/null
  MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1; exit 1
fi
sleep 3

SSH="/c/Windows/System32/OpenSSH/ssh.exe -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR -o ConnectTimeout=10 -p $PORT toby@127.0.0.1"
PASS=0; FAIL=0
run() {   # run <label> <remote-cmd> <required-substring>
  local out; out=$($SSH "$2" 2>&1 | tr -d '\r')
  if echo "$out" | grep -qF "$3"; then
      echo "  ok   $1"; PASS=$((PASS+1))
  else
      echo "  FAIL $1 -- wanted '$3', got: $(echo "$out" | head -2 | tr '\n' ' ')"; FAIL=$((FAIL+1))
  fi
}

echo "=== things the old term.c interpreter could NOT do ==="
run "pipeline"        'echo pipeworks | cat'                        "pipeworks"
run "two-stage pipe"  'echo one two three | wc -w'                  "3"
run "redirect + read" 'echo redir > /tmp/r.txt; cat /tmp/r.txt'     "redir"
run "variable"        'X=abc123; echo $X'                           "abc123"
run "external prog"   'ls /bin | head -3'                           "["

# Exit status needs the CODE, not stdout -- an empty expectation matches errors.
$SSH 'exit 7' >/dev/null 2>&1; rc=$?
if [ "$rc" = "7" ]; then echo "  ok   exit status"; PASS=$((PASS+1))
else echo "  FAIL exit status -- wanted rc=7, got rc=$rc"; FAIL=$((FAIL+1)); fi

echo "=== the case originally reported ==="
run "cat file | less" 'cat /etc/resolv.conf | less'                 "nameserver"

echo
echo "=== serial evidence ==="
grep -aE "tsh pid=|pty-req accepted|session shell pid" "$LOG" | sed 's/^\[[0-9 ]*ms\] //' | head -6
echo "=== kernel health (must be 0) ==="
echo "  idle stalls: $(grep -ac 'idle_stall' "$LOG")"
echo "  panics:      $(grep -ac 'PANIC' "$LOG")"

MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
echo; echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
