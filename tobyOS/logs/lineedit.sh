#!/bin/bash
# tsh line editing over a REAL pty.
#
# ssh -tt forces a pty and forwards bytes raw, so escape sequences reach the
# editor exactly as a keyboard would send them. Without -tt there is no pty,
# tsh_lineedit_available() is false, and this would silently exercise the
# plain reader instead -- the failure mode the test exists to catch.
#
# TWO ASSERTION RULES, both learned the hard way:
#
#  1. Match the COMMAND OUTPUT as a whole line (grep -x). The editor repaints
#     the line on every keystroke, so everything typed appears in the stream
#     as echo. A bare substring match cannot tell "the edit worked" from "the
#     characters went past on their way to being deleted".
#  2. Forbid the string the FAILED edit would produce, never the text typed.
#     Forbidding 'bad' after ^W fails always, because 'bad' was echoed while
#     being typed; forbidding 'badnice' is the real discriminator.
#
# Input is PACED. Piping the whole script at once races the shell: keystrokes
# meant for the editor can arrive while a command is still running and the tty
# is back in canonical mode.
cd /c/CustomOS/tobyOS || exit 1
LOG=logs/letest.log
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
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

# "listening" is NOT ready. sshd binds ~14s in, while the desktop, login and
# the rest of boot are still starting; sessions opened in that window get
# accepted and then serviced too slowly, and the client gives up during the
# banner exchange. That looked like editing failures, but only ever hit the
# FIRST few cases -- the later ones passed on the same binary.
#
# So wait for a signal that boot has actually finished (the desktop) and for
# the heartbeat to tick a few times, rather than guessing at a sleep.
for i in $(seq 1 30); do grep -aq "Desktop ready" "$LOG" 2>/dev/null && break; sleep 1; done
for i in $(seq 1 30); do
    [ "$(grep -ac '\[hb\]' "$LOG")" -ge 4 ] && break
    sleep 1
done
sleep 2

SSH="/c/Windows/System32/OpenSSH/ssh.exe -tt -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR -o ConnectTimeout=10 -p 2222 toby@127.0.0.1"
PASS=0; FAIL=0

# A stale ssh.exe from an aborted run holds a session, and the server serves
# ONE at a time -- a second connection then sits unanswered until it times
# out, which reads as an editing failure. Clear them, then take one throwaway
# session so the first ASSERTED one is not also the first connect after boot.
MSYS_NO_PATHCONV=1 taskkill /F /IM ssh.exe >/dev/null 2>&1
sleep 1
(printf 'exit\n' | timeout 20 $SSH >/dev/null 2>&1) || true
sleep 2

# le <label> <want-exact-line> <must-not-appear> <chunk> [<chunk>...]
# Each chunk is printf'd then followed by a pause, so the editor is reading
# when the bytes land.
le() {
    local label="$1" want="$2" nope="$3"; shift 3
    local out
    # The 1.5s lead-in is not padding: without it the first chunk lands before
    # tsh has reached its first readline and is dropped, which showed up as
    # random failures that moved between runs.
    out=$( { sleep 1.5; for c in "$@"; do printf "$c"; sleep 0.7; done; printf 'exit\n'; sleep 0.5; } \
           | timeout 30 $SSH 2>&1 | tr -d '\r' )
    # Let the server finish tearing the session down before the next case.
    # It serves ONE session at a time by design, so back-to-back connections
    # race the previous teardown -- the next one is either refused or sits
    # unserviced until it times out. That is a property of the server, not a
    # flake, so the harness paces itself rather than pretending otherwise.
    sleep 2
    # Exact-line, like the positive check: the typed text is echoed by the
    # repaint, so only a whole OUTPUT line proves the edit did not happen.
    if [ -n "$nope" ] && echo "$out" | grep -qx "$nope"; then
        echo "  FAIL $label -- '$nope' present, so the edit did not take"
        FAIL=$((FAIL+1)); return
    fi
    if echo "$out" | grep -qx "$want"; then
        echo "  ok   $label"; PASS=$((PASS+1))
    else
        echo "  FAIL $label -- no line equal to '$want'; tail: $(echo "$out" | tr '\n' '|' | tail -c 200)"
        FAIL=$((FAIL+1))
    fi
}

echo "=== history ==="
# Run it, then Up + Enter runs it again -> the output line appears TWICE.
out=$( { sleep 1.5; printf 'echo alpha\n'; sleep 1; printf '\033[A\n'; sleep 1; printf 'exit\n'; sleep 0.5; } \
       | timeout 30 $SSH 2>&1 | tr -d '\r' )
sleep 2
c=$(echo "$out" | grep -cx 'alpha')
if [ "$c" -ge 2 ]; then echo "  ok   Up recalls and re-runs (alpha x$c)"; PASS=$((PASS+1))
else echo "  FAIL Up recalls -- 'alpha' on its own line $c time(s), want >=2"; FAIL=$((FAIL+1)); fi

le "Up twice reaches the OLDER command" 'one' '' \
   'echo one\n' 'echo two\n' '\033[A\033[A\n'
le "^P recalls like Up"                 'ctrlp' '' \
   'echo ctrlp\n' '\020\n'

echo "=== editing bindings ==="
le "^A moves to start of line"          'world' '' \
   'world' '\001echo \n'
le "^W kills the previous word"         'good nice' 'good badnice' \
   'echo good bad' '\027nice\n'
le "^U discards the line"               'clean' 'garbageecho' \
   'garbage' '\025echo clean\n'
le "Left x3 + ^K kills to end"          'AAA-' 'AAA-BBB' \
   'echo AAA-BBB' '\033[D\033[D\033[D' '\013\n'
le "^A then ^E returns to the end"      'tail done' '' \
   'echo tail' '\001\005 done\n'
le "ESC-b moves back one word"          'ZZaa bb' '' \
   'echo aa bb' '\033b\033b' 'ZZ\n'
le "^Y yanks back what ^W killed"       'dup dup' '' \
   'echo dup ' '\027' '\031\031\n'

echo "=== the plain path still works (no pty) ==="
NOTTY="/c/Windows/System32/OpenSSH/ssh.exe -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR -p 2222 toby@127.0.0.1"
o=$(timeout 20 $NOTTY 'echo notty | cat' 2>&1 | tr -d '\r' | head -1)
if [ "$o" = "notty" ]; then echo "  ok   non-tty falls back to the plain reader"; PASS=$((PASS+1))
else echo "  FAIL non-tty path -- got '$o'"; FAIL=$((FAIL+1)); fi

echo
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'PANIC' "$LOG")"
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
