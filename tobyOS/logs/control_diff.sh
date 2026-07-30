#!/bin/bash
# Slice 72: show what a HEALTHY chrome startup does around/after the point
# where tobyOS's chrome dies (its last output is the 2nd GetCollectStatsConsent
# line, then INT3). Everything printed after that marker is startup work
# tobyOS never reaches -- the diff to investigate.
F=/mnt/c/CustomOS/tobyOS/logs/control/err_tobyflags.txt
echo "=== total stderr lines: $(wc -l <$F)"
N=$(grep -n GetCollectStatsConsent "$F" | sed -n 2p | cut -d: -f1)
echo "=== 2nd consent line at $N; the next 24 lines (tobyOS never gets here):"
sed -n "$((N+1)),$((N+24))p" "$F" | cut -c1-160
echo
echo "=== any CHECK/FATAL anywhere in the healthy run:"
grep -aiE 'check failed|FATAL|ImmediateCrash|received signal' "$F" | head -5 | cut -c1-160
echo "(none above = chrome never CHECKs on known-good Linux)"
