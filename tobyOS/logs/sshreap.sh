#!/bin/bash
# sshreap.sh -- the SSH server's single session slot, and what happens to it.
#
#   bash logs/sshreap.sh [--noboot]
#
# WHY THIS GATE EXISTS. The real EliteDesk wedged twice: it answered ICMP,
# accepted TCP 22, and served nobody. It was filed as "an abandoned session
# holds the only slot forever", with no reproduction. This gate reproduces
# it over SLIRP hostfwd using the host's own OpenSSH client -- the same
# client edremote drives at the real box -- and the cause turned out to be
# simpler and more specific than the note:
#
#   1. THE SERVER THREW AWAY SSH_MSG_CHANNEL_EOF. `ssh host 'cat > file'
#      < local` is how edremote uploads a script. The data arrived, the
#      client said "that is all the stdin there is", and the handler was a
#      bare `return`. cat then waited on a stdin that never ended: measured
#      here at 33 MINUTES before the client was killed. The session never
#      finished, and the server has exactly one slot. Short commands never
#      redirect stdin, which is precisely why only uploads ever wedged it.
#
#   2. UPLOAD INTEGRITY. The channel window advertised at channel-open is
#      32 KiB and was never accounted for again, while the pty input ring it
#      drains into is PTY_BUF = 4 KiB -- and ring_push() dropped on overflow
#      while pty_master_write() reported the full count. So a client was
#      invited to send eight times what could be held, and the middle of a
#      large upload vanished with nothing anywhere saying so. That is the
#      reported "hwgate's 68-section script fails its md5 check".
#
#   3. SLOT RECOVERY. ssh_channel_teardown() closed the pty master but never
#      killed the shell it had spawned. A shell blocked in a pty read gets
#      its EOF from that close; one blocked anywhere else (`sleep`, say) does
#      not, and keeps the pty slave open. There are PTY_MAX = 8 slots.
#
# --noboot reuses the ISO and a QEMU already running from a previous run.
set -u
cd /c/CustomOS/tobyOS || exit 1
LOG=logs/sshreap.log
WORK=logs/.sshreap
NOBOOT=0
[ "${1:-}" = "--noboot" ] && NOBOOT=1

SSHEXE=/c/Windows/System32/OpenSSH/ssh.exe
[ -x "$SSHEXE" ] || { echo "ABORT: no host OpenSSH client at $SSHEXE"; exit 1; }

if [ $NOBOOT -eq 0 ]; then
    MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
    MSYS_NO_PATHCONV=1 taskkill /F /IM ssh.exe >/dev/null 2>&1
    sleep 2
    if [ "$(MSYS_NO_PATHCONV=1 netstat -ano 2>/dev/null | grep -c ':2222')" != "0" ]; then
        echo "ABORT: port 2222 busy"; exit 1
    fi
    rm -f "$LOG" || { echo "ABORT: log busy"; exit 1; }

    # REBUILD THE WHOLE KERNEL, not just kernel.o.
    #
    # The standing rule in this tree is "rm src/kernel.o before a build whose
    # -D flags differ", because EXTRA_CFLAGS is not a file and make cannot
    # see that it changed. That is enough for a flag only kernel.c reads. It
    # is NOT enough for one like -DSMP_DIAG that half of src/ reads: a run of
    # this gate with SSHREAP_CFLAGS=-DSMP_DIAG followed by one without left
    # an ISO whose objects disagreed about the flag, and it hung during boot
    # at audioplay -- before a single session, so it looked like a kernel
    # regression rather than a stale build. src/ alone is a couple of
    # minutes; the third-party and libtoby trees are untouched.
    rm -f src/*.o
    # SSHREAP_CFLAGS=-DSMP_DIAG names the BKL owner in a watchdog report
    # instead of printing -2.
    #
    # IT MUST BE APPENDED TO THE DEFAULT, NEVER SUBSTITUTED FOR IT. The
    # Makefile says `EXTRA_CFLAGS ?= -DFAST_BOOT -DQUICK_BOOT`, so naming
    # EXTRA_CFLAGS on the command line at all replaces those -- and the guest
    # then takes the full slow-boot path, runs the M26A audio self-tests, and
    # hangs in audioplay before reaching a single SSH session. That looked
    # exactly like a kernel regression from the change under test.
    PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
      make "CC=TMP='C:\t' TEMP='C:\t' clang" \
           "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
           EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT ${SSHREAP_CFLAGS:-}" \
           iso >logs/.buildlog.txt 2>&1
    if [ $? -ne 0 ]; then
        echo "ABORT: the build FAILED -- any verdict below would describe"
        echo "       code that is not in the ISO on disk."
        tail -30 logs/.buildlog.txt; exit 1
    fi

    "/c/Program Files/qemu/qemu-system-x86_64.exe" \
      -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
      -boot d -smp 4 -m 5120 -cpu max \
      -netdev user,id=net0,hostfwd=tcp::2222-:22 \
      -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
      -serial file:"$LOG" -no-reboot -display none 2>/dev/null &

    for i in $(seq 1 60); do grep -aq "ssh\] listening" "$LOG" 2>/dev/null && break; sleep 2; done
    if ! grep -aq "ssh\] listening" "$LOG" 2>/dev/null; then
        echo "ABORT: sshd never came up"
        tail -20 "$LOG"
        MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
        exit 1
    fi
    for i in $(seq 1 30); do grep -aq "Desktop ready" "$LOG" 2>/dev/null && break; sleep 1; done
    sleep 3
fi

SSH="$SSHEXE -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR -o ConnectTimeout=10 -p 2222 toby@127.0.0.1"

# EVERY session gets a deadline. The first version of this gate did not, and
# the very first upload hung for 33 minutes -- which was the bug, but it also
# meant the run never reached the other cases. A hang has to be a FAILING
# case, not a stalled harness.
sshrun() {   # sshrun <seconds> <remote command>   (stdin is passed through)
    MSYS_NO_PATHCONV=1 timeout -k 3 "$1" $SSH "$2" 2>&1 | tr -d '\r'
}
reap_clients() { MSYS_NO_PATHCONV=1 taskkill /F /IM ssh.exe >/dev/null 2>&1; sleep 1; }

rm -rf "$WORK"; mkdir -p "$WORK"
PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  FAIL %s -- %s\n' "$1" "$2"; }

echo "=== the channel answers at all ==="
OUT=$(sshrun 20 'echo hello-from-guest')
if [ "$OUT" = "hello-from-guest" ]; then ok "a plain exec session"
else bad "a plain exec session" "got '$OUT'"; echo "  (nothing below can be trusted; stopping)"
     MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1; exit 1; fi

echo
echo "=== 1. stdin reaches its end: ssh host 'cat > file' < local ==="
# This is edremote's upload, and it is the case that wedged the real box:
# the client sends the data then SSH_MSG_CHANNEL_EOF, which the server used
# to acknowledge and discard, so cat waited on a stdin that never ended and
# the server's one session slot was held forever.
for SZ in 1000 4000 8000 20000; do
    F="$WORK/up_$SZ.bin"
    awk -v n="$SZ" 'BEGIN{s="";while(length(s)<n)s=s "abcdefghij0123456789";printf "%s", substr(s,1,n)}' > "$F"
    MSYS_NO_PATHCONV=1 timeout -k 3 45 $SSH "cat > /tmp/up_$SZ" < "$F" >/dev/null 2>&1
    UPRC=$?
    reap_clients
    if [ $UPRC -eq 124 ] || [ $UPRC -eq 137 ]; then
        bad "$SZ bytes: the upload session ends" "timed out -- stdin EOF never delivered"
    else
        ok "$SZ bytes: the upload session ends"
    fi
    GOT=$(sshrun 20 "wc -c < /tmp/up_$SZ" | tr -d ' ')
    if [ "$GOT" = "$SZ" ]; then ok "$SZ bytes arrive intact"
    else bad "$SZ bytes arrive intact" "guest received '$GOT' of $SZ"; fi
    HEAD=$(sshrun 20 "head -c 20 /tmp/up_$SZ")
    WANT=$(head -c 20 "$F")
    [ "$HEAD" = "$WANT" ] && ok "$SZ bytes: the content is right" \
                          || bad "$SZ bytes: the content is right" "head '$HEAD' vs '$WANT'"
done

echo
echo "=== 2. a session whose client walks away frees its pty ==="
# `sleep` blocks in a syscall, NOT in a pty read, so closing the pty master
# does not disturb it. Before the teardown learned to kill the shell, each
# of these rounds leaked one of the PTY_MAX = 8 slots and the run died on
# round 9 with "no free pty".
for ROUND in 1 2 3 4 5 6 7 8 9 10; do
    MSYS_NO_PATHCONV=1 timeout -k 2 6 $SSH 'sleep 300' >/dev/null 2>&1
    reap_clients
    OUT=$(sshrun 20 "echo alive-$ROUND")
    if [ "$OUT" = "alive-$ROUND" ]; then ok "round $ROUND: the slot came back"
    else bad "round $ROUND: the slot came back" "got '$OUT'"; fi
done

echo
echo "=== 3. the guest is still healthy ==="
OUT=$(sshrun 20 'echo still-here')
[ "$OUT" = "still-here" ] && ok "a session still works at the end" \
                          || bad "a session still works at the end" "got '$OUT'"
NOPTY=$(grep -ac 'no free pty' "$LOG" 2>/dev/null)
[ "${NOPTY:-0}" = "0" ] && ok "no 'no free pty' in the kernel log" \
                        || bad "no 'no free pty' in the kernel log" "$NOPTY occurrences"
OVR=$(grep -ac 'channel input overrun' "$LOG" 2>/dev/null)
[ "${OVR:-0}" = "0" ] && ok "no channel input overrun" \
                      || bad "no channel input overrun" "$OVR occurrences"
STALL=$(grep -ac 'idle_stall' "$LOG" 2>/dev/null)
[ "${STALL:-0}" = "0" ] && ok "pid 0 never stalled" \
                        || bad "pid 0 never stalled" "$(grep -a 'idle_stall' "$LOG" | head -1 | sed 's/.*reason=//')"
PANIC=$(grep -ac 'PANIC' "$LOG" 2>/dev/null)
[ "${PANIC:-0}" = "0" ] && ok "no panics" || bad "no panics" "$PANIC"

echo
echo "SSHREAP: $PASS ok, $FAIL failed"
if [ $NOBOOT -eq 0 ]; then
    MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
fi
[ "$FAIL" -eq 0 ]
