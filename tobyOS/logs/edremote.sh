#!/bin/bash
# edremote.sh -- run a command on the EliteDesk and bring the output back here.
#
# The point of this is to stop the real-hardware loop being "user boots the
# box, reads serial, pastes a log". With this, a command runs on the actual
# machine and its stdout, stderr and exit status come back to the dev box,
# so a real-HW test is the same shape as a QEMU one.
#
# The channel is tobyOS's own SSH server:
#   - src/ssh.c handles an "exec" channel request, so `ssh host 'cmd'` works
#     without an interactive session.
#   - the session is a real /bin/tsh on a real pty, so pipes, redirection and
#     external programs all behave (the old term.c path could not do that).
#   - SSH_AUTH_METHODS is "none": a valid username is the whole check. That is
#     fine on a lab LAN and NOT fine anywhere else. See the note at the end.
#   - logs/sshtsh.sh already proves this server interoperates with the Windows
#     OpenSSH client, so this reuses that client and its options verbatim.
#
# Usage:
#   bash logs/edremote.sh 'uname -a; cat /proc/stat | head -3'
#   bash logs/edremote.sh -f script.sh      # run a local script's contents
#   bash logs/edremote.sh --check           # preflight only
#   ED_HOST=192.168.68.91 bash logs/edremote.sh 'ls'
#
# Exit status is the REMOTE command's, except for the reserved codes below,
# so `if bash logs/edremote.sh 'test -f /bin/tsh'; then` reads correctly.
#   90 host unreachable   91 SSH port shut   92 services not answering
#   93 usage error
set -u

ED_HOST="${ED_HOST:-192.168.68.90}"
ED_USER="${ED_USER:-toby}"
SSH_BIN="${SSH_BIN:-/c/Windows/System32/OpenSSH/ssh.exe}"
TIMEOUT="${ED_TIMEOUT:-60}"

SSH_OPTS=(-o StrictHostKeyChecking=no
          -o UserKnownHostsFile=NUL
          -o LogLevel=ERROR
          -o ConnectTimeout=10
          -o NumberOfPasswordPrompts=0)

usage() {
    sed -n '2,26p' "$0" | sed 's/^# \?//'
    exit 93
}

# ---- preflight -----------------------------------------------------------
# Three failure modes that look identical from `ssh` alone but mean very
# different things. Naming them is the whole reason this function exists:
# "timed out" sent me looking at SSH once when the answer was that pid 0's
# service lane had stopped running.
preflight() {
    if ! ping -n 2 -w 1000 "$ED_HOST" >/dev/null 2>&1; then
        echo "edremote: $ED_HOST does not answer ICMP -- powered off, not" >&2
        echo "          booted yet, or on a different address than expected." >&2
        return 90
    fi

    # TCP connect and then wait for the SSH version banner. tobyOS's net stack
    # completes the handshake from the RX path, but the banner is written by a
    # service that only runs inside pid 0's net_service_tick. So "connects but
    # never speaks" specifically means the kernel is alive and pid 0's driver
    # loop is not -- the signature of the open idle-stall flake.
    python - "$ED_HOST" <<'PY'
import socket, sys
host = sys.argv[1]
s = socket.socket(); s.settimeout(4)
try:
    s.connect((host, 22))
except Exception as e:
    print("SHUT %s" % e); sys.exit(0)
s.settimeout(12)
try:
    b = s.recv(64)
except Exception:
    print("SILENT"); sys.exit(0)
finally:
    s.close()
print("BANNER %s" % (b.decode("utf8", "replace").strip() if b else "EMPTY"))
PY
}

check() {
    local r
    r=$(preflight) || return $?
    case "$r" in
      SHUT*)   echo "edremote: $ED_HOST is up but nothing is listening on 22." >&2
               echo "          ($r)" >&2
               return 91 ;;
      SILENT)  echo "edremote: $ED_HOST accepts TCP on 22 but sends no SSH banner." >&2
               echo "          The kernel is alive (it answers ping and completes the" >&2
               echo "          handshake from the RX path) but NO network service is" >&2
               echo "          running. Every service is driven by net_service_tick()" >&2
               echo "          inside pid 0's loop, so this means that loop is stalled" >&2
               echo "          or starved. Check the serial console: if the [hb]" >&2
               echo "          heartbeat has stopped, pid 0 is wedged and the box needs" >&2
               echo "          a reboot. See the wake-after-death idle-stall item." >&2
               return 92 ;;
      BANNER*) echo "edremote: $ED_HOST ready -- ${r#BANNER }" >&2
               return 0 ;;
      *)       echo "edremote: unexpected preflight result '$r'" >&2
               return 92 ;;
    esac
}

# ---- main ----------------------------------------------------------------
[ $# -ge 1 ] || usage

if [ "$1" = "--check" ]; then check; exit $?; fi

if [ "$1" = "-f" ]; then
    [ $# -eq 2 ] || usage
    [ -r "$2" ] || { echo "edremote: cannot read '$2'" >&2; exit 93; }
    CMD=$(cat "$2")
else
    CMD="$*"
fi

check || exit $?

# -T: no pty for the exec channel. The server spawns tsh on a pty regardless,
# but asking the client not to allocate one keeps the output free of the
# terminal escapes that would otherwise land in whatever parses this.
run() { timeout "$TIMEOUT" "$SSH_BIN" "${SSH_OPTS[@]}" -T "$ED_USER@$ED_HOST" "$@"; }

# ---- the 512-byte wall, and how this gets past it ------------------------
# ssh.c holds the exec command in a 4096-byte buffer and deliberately REFUSES
# anything longer rather than truncating it. That guard is defeated one layer
# down: the command is handed to `tsh -c <string>` as a single argv element,
# and the spawn path caps an element at ABI_ARG_MAX, which is 512. So a
# command between 512 and 4095 bytes is silently cut in half and usually
# produces NO output at all, which is what a multi-line test script looks
# like. Measured on real hardware: 500 bytes fine, 660 bytes silent.
#
# Rather than wait for a kernel fix, anything long goes over STDIN instead --
# the exec channel forwards it, so `cat > file` uploads byte-for-byte (md5
# verified on a real box). Short commands still take the direct path because
# it is one round trip instead of three.
if [ ${#CMD} -le 400 ]; then
    run "$CMD"
    rc=$?
else
    REMOTE="/tmp/edr_$$.sh"
    LOCAL=$(mktemp)
    printf '%s
' "$CMD" > "$LOCAL"
    want=$(md5sum < "$LOCAL" | cut -d' ' -f1)

    # The upload's own exit status is NOT trustworthy: redirecting stdin into
    # the exec channel and closing it returns non-zero even when every byte
    # arrived intact (measured -- the file was byte-perfect and the status was
    # still non-zero). So ignore it and let the checksum below be the only
    # judge. Verifying the artifact beats believing the transport.
    run "cat > $REMOTE" < "$LOCAL" >/dev/null 2>&1 || true
    got=$(run "md5sum < $REMOTE" 2>/dev/null | tr -d '' | cut -d' ' -f1)
    rm -f "$LOCAL"
    if [ "$got" != "$want" ]; then
        echo "edremote: uploaded script does not match" >&2
        echo "          local  $want" >&2
        echo "          remote ${got:-<nothing>}" >&2
        exit 92
    fi
    run "tsh $REMOTE; __rc=\$?; rm -f $REMOTE; exit \$__rc"
    rc=$?
fi
[ $rc -eq 124 ] && echo "edremote: remote command exceeded ${TIMEOUT}s" >&2
exit $rc

# ---- two known hazards ---------------------------------------------------
# 1. NEVER kill this script with an external `timeout` while a session is
#    open. tobyOS's SSH server handles one session at a time and an abandoned
#    one leaves it holding a peer that will never speak again. Use ED_TIMEOUT,
#    which bounds the REMOTE command, instead of killing the client.
# 2. Session churn appears to trigger the open idle-stall flake: after roughly
#    25-30 sessions the box stopped answering any service and needed a reboot.
#    Batch work into ONE invocation rather than many small ones. If --check
#    starts returning 92, that is what happened. See the flake memo.

# ---- a word on the auth model -------------------------------------------
# This works because the SSH server accepts method "none". That is a
# deliberate lab convenience and it means ANYONE on the LAN who knows a
# username has a shell. It is fine for a bench machine on a home network and
# it is not a configuration to ship. If tobyOS ever leaves the bench, the
# server needs real publickey auth before it leaves.
