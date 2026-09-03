#!/bin/bash
# edchurn.sh -- how many SSH sessions does the real box survive before pid 0's
# service lane stops answering?
#
# The idle-stall flake has been open a long time and its reproduction was
# "use the machine for a while". Two real-hardware wedges on 2026-09-01/02
# pointed at process churn: every `ssh host cmd` session spawns a /bin/tsh,
# runs it, exits and reaps it, so a session IS a churn event, and a loop of
# them is a churn generator with a counter attached.
#
# This turns the flake into a number: the iteration it died on. A number can
# be compared across builds, which "it wedged eventually" cannot.
#
#   bash logs/edchurn.sh [max_iterations]     # default 200
#
# Exit 0  survived every iteration (record the count -- that is the result)
# Exit 1  wedged (the iteration number is the measurement)
# Exit 90 the box was already unhealthy before the run started
#
# ONE ssh connection per iteration. The banner probe only runs when a command
# fails, so a healthy loop costs exactly one session per count. Deliberately
# NOT using an external kill to bound a session: an abandoned session was
# tested and exonerated, but there is no reason to add a second variable to
# an experiment that is measuring one thing.
set -u

MAX="${1:-200}"
ED_HOST="${ED_HOST:-192.168.68.90}"
ED_USER="${ED_USER:-toby}"
SSH_BIN="${SSH_BIN:-/c/Windows/System32/OpenSSH/ssh.exe}"
HERE="$(cd "$(dirname "$0")" && pwd)"

SSH_OPTS=(-o StrictHostKeyChecking=no
          -o UserKnownHostsFile=NUL
          -o LogLevel=ERROR
          -o ConnectTimeout=10
          -o NumberOfPasswordPrompts=0)

echo "edchurn: target $ED_HOST, up to $MAX sessions"
if ! bash "$HERE/edremote.sh" --check >/dev/null 2>&1; then
    echo "edchurn: box is not healthy BEFORE the run -- nothing to measure" >&2
    bash "$HERE/edremote.sh" --check
    exit 90
fi
echo "edchurn: healthy at start"
START=$(date +%s)

for i in $(seq 1 "$MAX"); do
    # A trivial command: the point is the session lifecycle (spawn tsh, run,
    # exit, reap), not the work. 45 s is far longer than a healthy round trip
    # (about 2 s), so hitting it means something is already wrong.
    out=$(timeout 45 "$SSH_BIN" "${SSH_OPTS[@]}" -T "$ED_USER@$ED_HOST" \
          'echo ok' 2>&1 | tr -d '\r')
    rc=$?

    if [ "$rc" -eq 0 ] && [ "$out" = "ok" ]; then
        # Progress without flooding: every 10th, plus the first three.
        if [ "$i" -le 3 ] || [ $((i % 10)) -eq 0 ]; then
            echo "  [$i] ok  ($(( $(date +%s) - START ))s elapsed)"
        fi
        continue
    fi

    echo
    echo "edchurn: session $i FAILED  rc=$rc  output='$out'"
    echo "edchurn: classifying..."
    bash "$HERE/edremote.sh" --check
    ck=$?
    ELAPSED=$(( $(date +%s) - START ))
    echo
    echo "=========================================================="
    echo " WEDGED AFTER $i SESSIONS  (${ELAPSED}s)"
    if [ "$ck" -eq 92 ]; then
        echo " Classification: pid 0's service lane is dead -- the known"
        echo " idle-stall signature. THIS COUNT IS THE MEASUREMENT."
    else
        echo " Classification: preflight exit $ck, NOT the usual signature."
        echo " Read the codes in edremote.sh before assuming it is the flake."
    fi
    echo "=========================================================="
    exit 1
done

ELAPSED=$(( $(date +%s) - START ))
echo
echo "edchurn: survived all $MAX sessions in ${ELAPSED}s -- no wedge."
echo "         That is a real result: record the count. If a previous run"
echo "         wedged at a lower number, whatever changed between them is"
echo "         the thing to look at."
exit 0
