#!/bin/bash
# devoil.sh TAG -- the whole Oils corpus on the RUNNING guest ($ED_HOST etc.,
# as edremote.sh takes them) against whatever /bin/tsh is installed there
# (tools/agent/tshup.sh), with no ISO rebuild. Reported against a DEV lineage
# ($DEVOIL_DIR/oilspec_prev_dev.json, default logs/b12/) so neither the QEMU
# gate's archive nor the hardware one moves. Everything lands in
# $DEVOIL_DIR/devoil-TAG.*: the raw log, the report, the diff, the failures.
# Proves the HOSTED shell only, as tshup.sh does.
cd "$(dirname "$0")/../.." || exit 1
TAG=${1:?TAG}
D=${DEVOIL_DIR:-logs/b12}
OUT=$D/devoil-$TAG.log
START=$(date +%s)
ED_TIMEOUT=${ED_TIMEOUT:-5400} bash logs/edremote.sh 'oilspec' > "$OUT" 2> "$D/devoil-$TAG.err" < /dev/null
echo "devoil: rc=$? after $(( $(date +%s) - START ))s, $(wc -c < "$OUT") bytes"
OILSPEC_PREV=$D/oilspec_prev_dev.json OILSPEC_FAILOUT=$D/devoil-$TAG.fail.txt \
    python logs/oilspec_report.py "$OUT" > "$D/devoil-$TAG.report.txt" 2>&1
cp logs/oilspec_diff.txt "$D/devoil-$TAG.diff.txt" 2>/dev/null
grep -a 'OILSPEC\] VERDICT' "$OUT" || echo "devoil: NO VERDICT"
grep -a -E "POSIX  |BASH-ONLY|gained|LOST" "$D/devoil-$TAG.report.txt" | head -30
