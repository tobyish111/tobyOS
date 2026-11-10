#!/bin/bash
# cp, and the copy-naming rule.
#
#   bash logs/cpcopy.sh
#
# THREE parts:
#
#  1. HOST: the naming rule itself (logs/uniquename_test.c), compiled straight
#     from <tobyos/unique_name.h> -- the same code /bin/cp -U and File
#     Explorer's Duplicate both call.
#
#  2. PARITY: one scenario run twice -- under real GNU bash with coreutils cp
#     on this machine, and under /bin/tsh with tobyOS cp in the guest -- and
#     the two transcripts diffed. That is the only honest way to answer "is cp
#     supported in tsh like it is in bash": not by reading the flag list, by
#     running the same commands and comparing every byte.
#
#  3. UNIQUE: -U alone, guest only, since coreutils has no such flag.
#
# TWO HARNESS TRAPS, both of which cost a debugging cycle and NEITHER of which
# was a tobyOS bug:
#
#   - `&&` does not survive this host's MSYS layer. One arrived in the guest
#     as a command literally named 'C:/Program', which tsh dutifully failed to
#     launch. Use `;` only. (shparity proves && works in tsh, 110/110.)
#   - `ssh host "cat > file"` HANGS: stdin EOF is never delivered to the guest
#     process, so cat waits forever and holds the one session this server
#     serves -- every later session then times out during banner exchange,
#     which looks like a network fault and is not. Do not transfer files that
#     way; build them with shell redirects.
#
# The scenario uses globbing and cat rather than `ls`: ls output formatting is
# its own parity question, and would make a cp bug and an ls bug look alike.
cd /c/CustomOS/tobyOS || exit 1
LOG=logs/cpcopy.log

echo "=== host: the naming rule ==="
PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
  gcc -std=c11 -Wall -Wextra -I include \
      -o logs/uniquename_test.exe logs/uniquename_test.c || {
    echo "ABORT: naming test did not compile"; exit 1; }
./logs/uniquename_test.exe
HOST_RC=$?

# Single-quoted heredoc: the text must reach bash and tsh byte-identical.
# `;` only -- see the && trap above. WORKDIR is substituted per side.
# FOUR short chunks, not two long ones: an ssh exec command is truncated
# somewhere past ~500 bytes (measured: a 600-byte command died mid-stream),
# and a truncated script ends inside a quote, so tsh reports "unmatched
# double quote" and the run looks like a shell bug. Setup and listing are
# separate sessions; the files persist in /tmp for the life of the boot.
SC_SETUP='cd WORKDIR;mkdir -p case;cd case;mkdir -p d1 d2 tree/sub;echo AAA > a.txt;echo BBB > b.txt;echo X > tree/sub/x.txt;cp a.txt c.txt;cp a.txt d1/;cp a.txt b.txt d2/;cp -r tree tree2;cp -n a.txt b.txt;cp a.txt c.txt'
SC_LIST='cd WORKDIR/case;echo "-- top --";for f in *; do echo $f; done;echo "-- d1 --";for f in d1/*; do echo $f; done;echo "-- d2 --";for f in d2/*; do echo $f; done;echo "-- tree2 --";for f in tree2/sub/*; do echo $f; done;echo "-- b --";cat b.txt;echo "-- c --";cat c.txt'
UQ_SETUP='cd WORKDIR;mkdir -p uniq;cd uniq;echo ORIG > u.txt;echo OTHER > v.txt;cp -U v.txt u.txt;cp -U v.txt u.txt;mkdir -p ud;cp v.txt ud/;cp -U v.txt ud/'
UQ_LIST='cd WORKDIR/uniq;echo "-- names --";for f in *; do echo $f; done;echo "-- ud --";for f in ud/*; do echo $f; done;echo "-- orig --";cat u.txt;echo "-- copy1 --";cat u_1.txt'

echo
echo "=== bash + coreutils, on this machine ==="
HOSTWORK=$(mktemp -d)
bash -c "${SC_SETUP//WORKDIR/$HOSTWORK}" 2>&1
HOST_OUT=$(bash -c "${SC_LIST//WORKDIR/$HOSTWORK}" 2>&1)
bash -c "${UQ_SETUP//WORKDIR/$HOSTWORK}" >/dev/null 2>&1
rm -rf "$HOSTWORK"
echo "$HOST_OUT"

echo
echo "=== tsh + tobyOS cp, in the guest ==="
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
MSYS_NO_PATHCONV=1 taskkill /F /IM ssh.exe >/dev/null 2>&1
sleep 2
if [ "$(MSYS_NO_PATHCONV=1 netstat -ano 2>/dev/null | grep -c ':2222')" != "0" ]; then
    echo "ABORT: port 2222 busy"; exit 1
fi
rm -f "$LOG" || { echo "ABORT: log busy"; exit 1; }

"/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
  -boot d -smp 4 -m 5120 -cpu max \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -serial file:"$LOG" -no-reboot -display none 2>/dev/null &

for i in $(seq 1 45); do grep -aq "ssh\] listening" "$LOG" 2>/dev/null && break; sleep 2; done
if ! grep -aq "ssh\] listening" "$LOG" 2>/dev/null; then
    echo "ABORT: sshd never came up"
    MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1; exit 1
fi
# "listening" is not "ready": sshd binds while the desktop is still starting,
# and a session opened in that window is accepted but serviced too slowly for
# the client's banner timeout.
for i in $(seq 1 30); do grep -aq "Desktop ready" "$LOG" 2>/dev/null && break; sleep 1; done
for i in $(seq 1 30); do [ "$(grep -ac '\[hb\]' "$LOG")" -ge 4 ] && break; sleep 1; done
sleep 2

SSH="/c/Windows/System32/OpenSSH/ssh.exe -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR -o ConnectTimeout=10 -p 2222 toby@127.0.0.1"
run() { timeout 40 $SSH "$1" 2>&1 | tr -d '\r'; sleep 3; }
run "${SC_SETUP//WORKDIR//tmp}" > /dev/null
GUEST_OUT=$(run "${SC_LIST//WORKDIR//tmp}")
run "${UQ_SETUP//WORKDIR//tmp}" > /dev/null
UNIQ_OUT=$(run "${UQ_LIST//WORKDIR//tmp}")
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
echo "$GUEST_OUT"

PASS=0; FAIL=0
echo
echo "=== results ==="
if [ "$HOST_OUT" = "$GUEST_OUT" ]; then
    echo "  ok   tsh+cp matches bash+coreutils exactly"
    PASS=$((PASS+1))
else
    echo "  FAIL transcripts differ (bash < > tsh):"
    diff <(echo "$HOST_OUT") <(echo "$GUEST_OUT") | head -30
    FAIL=$((FAIL+1))
fi

echo "$UNIQ_OUT"
chk() {
    if echo "$UNIQ_OUT" | grep -qx "$2"; then echo "  ok   $1"; PASS=$((PASS+1))
    else echo "  FAIL $1 -- no line '$2'"; FAIL=$((FAIL+1)); fi
}
chk "-U makes report_1 style names"  "u_1.txt"
chk "-U counts up on the next clash" "u_2.txt"
chk "-U works into a directory"      "ud/v_1.txt"
# The whole point of -U is that the original survives.
if echo "$UNIQ_OUT" | sed -n '/-- orig --/,/-- copy1 --/p' | grep -qx "ORIG"; then
    echo "  ok   -U left the original alone"; PASS=$((PASS+1))
else
    echo "  FAIL -U overwrote the original"; FAIL=$((FAIL+1))
fi

echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL host_rule=$([ $HOST_RC = 0 ] && echo PASS || echo FAIL)"
[ "$FAIL" = "0" ] && [ "$HOST_RC" = "0" ] || exit 1
