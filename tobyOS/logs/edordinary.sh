#!/bin/bash
# edordinary.sh -- an hour of ORDINARY shell work on the real EliteDesk, every
# step stamped to /data, in ONE SSH session.
#
#   bash logs/edordinary.sh [iterations]      # default 1800 (~1 h at ~2 s each)
#
# The last remaining real-hardware wedge (the box answers ping and completes
# TCP handshakes, no service ever speaks; edremote --check exits 92) has been
# reached through a GUI editor session and through a plain chmod/stat line.
# Four hypotheses about it died to measurement in one week. This is the
# measurement that comes next: a loop of the work a user actually does --
# create, chmod, stat, a pipeline, a spawn, ps, rm -- with the iteration and
# the command about to run written to /data/ordinary.txt BEFORE each step.
# The USB stick survives the wedge and the reboot, so after a power cycle
# `cat /data/ordinary.txt | tail -1` names the iteration and the command.
#
# Exit 0  the loop finished and the box is still healthy
# Exit 1  the session ended early or the box is unhealthy afterwards
# Exit 90 the box was already unhealthy before the run
set -u
N="${1:-1800}"
cd "$(dirname "$0")/.." || exit 1

if ! bash logs/edremote.sh --check >/dev/null 2>&1; then
    echo "edordinary: box is not healthy BEFORE the run -- nothing to measure" >&2
    bash logs/edremote.sh --check
    exit 90
fi

REMOTE=logs/edordinary.remote.sh
{
    echo "N=$N"
    cat logs/edordinary.body.sh
} > "$REMOTE"

START=$(date +%s)
echo "edordinary: $N iterations, one session, stamps in /data/ordinary.txt"
# Bound the REMOTE command generously (never kill the client mid-session).
ED_TIMEOUT="${ED_TIMEOUT:-7200}" bash logs/edremote.sh -f "$REMOTE" > logs/edordinary.out 2>&1
rc=$?
ELAPSED=$(( $(date +%s) - START ))
LAST=$(tr -d '\r' < logs/edordinary.out | grep "^ORDINARY" | tail -1)
echo "edordinary: session rc=$rc after ${ELAPSED}s -- $LAST"

bash logs/edremote.sh --check
ck=$?
if [ "$LAST" = "ORDINARY-DONE $N" ] && [ $ck -eq 0 ]; then
    echo "=========================================================="
    echo " SURVIVED $N ITERATIONS OF ORDINARY WORK (${ELAPSED}s), box healthy"
    echo "=========================================================="
    exit 0
fi
echo "=========================================================="
if [ $ck -eq 92 ]; then
    echo " WEDGED (exit 92) after ${ELAPSED}s. Power-cycle the box, then:"
    echo "   bash logs/edremote.sh 'tail -3 /data/ordinary.txt'"
    echo " The last line is the iteration and the command it was about to run."
    echo " Then read the serial console for the [kpf] line, not the registers."
else
    echo " Session ended early (rc=$rc, preflight $ck) -- see logs/edordinary.out"
fi
echo "=========================================================="
exit 1
