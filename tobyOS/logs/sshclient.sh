#!/bin/bash
# The SSH CLIENT: ssh, ssh-keygen, ssh-add, scp, sftp.
#
#   bash logs/sshclient.sh
#
# tobyOS has had an SSH server for a long time and no way to connect OUT.
# This is the second of the six groups from the distro census.
#
# THE TEST IS A REAL HANDSHAKE. The client connects to tobyOS's own sshd over
# loopback, which drives every stage for real: version exchange, KEXINIT,
# curve25519 key agreement, the Ed25519 host-key SIGNATURE, key derivation,
# chacha20-poly1305 packet framing, userauth, channel open, exec, and the
# remote exit status. There is no mock anywhere in that path, and the server
# end is known-good because real OpenSSH talks to it every day.
#
# Loopback is not a weaker test than a second machine here: the client and
# server are separate processes speaking the wire protocol, and every
# cryptographic step has to be right or the connection simply fails. What it
# does NOT cover is interoperability with OpenSSH as a SERVER, which needs a
# second host; the algorithms were chosen to match what OpenSSH offers first
# (curve25519-sha256, ssh-ed25519, chacha20-poly1305@openssh.com) precisely
# so that test is possible later.
#
# Runs from the boot harness, not over SSH-from-the-host -- see
# logs/coreutils.sh for why sessions are the wrong vehicle.
cd /c/CustomOS/tobyOS || exit 1
LOG=logs/sshclient.log

MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
sleep 2
rm -f "$LOG" || { echo "ABORT: log busy"; exit 1; }

rm -f src/kernel.o
PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
  make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
       EXTRA_CFLAGS=-DCOREUTILS_SELFTEST iso >logs/.buildlog.txt 2>&1
BUILD_RC=$?
# A FAILED BUILD USED TO LOOK LIKE A PASSING GATE. make's output went to
# /dev/null and the only check was "does an ISO exist" -- which a PREVIOUS
# run's ISO satisfies. One compile error therefore aborted make, left the
# stale image in place, and this gate booted it and reported confident
# results about source that was never in the build. Seen for real: a broken
# char literal in head.c silently froze three gates on an hour-old ISO.
if [ $BUILD_RC -ne 0 ]; then
    echo "ABORT: the build FAILED -- the ISO on disk is stale, so any"
    echo "       verdict below would have described code that is not in it."
    tail -25 logs/.buildlog.txt
    exit 1
fi
[ -f tobyOS.iso ] || { echo "ABORT: build produced no ISO"; exit 1; }

# The guest needs its own network up for loopback SSH; user-mode networking
# is enough and costs nothing.
"/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
  -boot d -smp 4 -m 5120 -cpu max \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -serial file:"$LOG" -no-reboot -display none 2>/dev/null &

# The shared boot harness now runs six scripts (coreutils, extra,
# accounts, ssh, util, e2fs) and the ssh one alone takes ~40s of guest
# time. Waiting is cheap; a false ABORT costs a whole rebuild.
for i in $(seq 1 400); do grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null && break; sleep 2; done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
if ! grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: the harness never reported"; exit 1
fi

# A kernel diagnostic can interleave MID-LINE, not only at the start of
# one, so a filter that drops lines beginning with '[' leaves it behind and
# a perfectly correct tool looks broken. Cut an embedded diagnostic off
# where it starts, and swallow the blank line its own newline leaves.
kern_strip() {
    sed -E 's/^\[[0-9 ]+ms\] //' | grep -v '^\[' | awk '
        { if (match($0, /\[[0-9]+ ms\] \[[a-z_0-9]+\]/)) {
              frag = frag substr($0, 1, RSTART - 1); pending = 1; next }
          if (pending) {
              pending = 0
              if ($0 == "") { print frag; frag = ""; next }
              print frag $0; frag = ""; next }
          print }
        END { if (frag != "") print frag }' |
    sed -E 's/(.)(@@[a-z_])/\1\n\2/g'
}
OUT=$(sed -n '/@@keygen/,/@@END/p' "$LOG" | kern_strip | tr -d '\r')
if ! echo "$OUT" | grep -q "@@END"; then
    echo "ABORT: transcript incomplete"; echo "$OUT" | tail -5; exit 1
fi

section() { echo "$OUT" | awk -v k="@@$1" '$0==k{f=1;next} /^@@/{f=0} f'; }
PASS=0; FAIL=0
want() {
    local name="$1" label="$2" pat="$3" g
    g=$(section "$name" | tr '\n' '|')
    if echo "$g" | grep -q "$pat"; then echo "  ok   $label"; PASS=$((PASS+1))
    else echo "  FAIL $label -- got '$g' (wanted /$pat/)"; FAIL=$((FAIL+1)); fi
}

echo "=== ssh-keygen ==="
want keygen        "generates an ed25519 key"        'fingerprint\|saved in'
want keygen_files  "writes both halves"              'private-ok|public-ok|ssh-ed25519'
want keygen_refuses_passphrase "refuses -N rather than storing in the clear" 'not supported.*rc=1'
want keygen_refuses_rsa        "refuses key types it cannot make"           'only ed25519.*rc=1'

echo "=== the handshake, against our own sshd ==="
want exec        "runs a remote command"        'hello-from-remote'
want exec_status "propagates the remote status" 'rc=7'
want pipeline    "a remote pipeline works"      '3'
want knownhosts  "pins the host key"            '^1'

echo "=== failure paths ==="
want badport "a refused connection is reported" 'failed.*rc=255'
want sshadd  "ssh-add says there is no agent"   'authentication agent.*rc=2'
want sftp    "sftp says the remote has no subsystem" 'subsystem.*rc=1'

echo
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
