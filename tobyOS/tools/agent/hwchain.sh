#!/bin/bash
# hwchain.sh -- the standing hardware proof, in order: hwgate first (it needs a
# FRESH boot), then text, ssh churn, the Oils corpus, then every kernel-shell probe
# (kwedge last, inside kprobechain), with the latency marks (hwlatency.sh)
# before them. ED_HOST must be set. Run a COM4 capture
# alongside (logs/com4cap.ps1) and grep it for PANIC/DUAL/EXCEPTION/tlb] fragments.
cd "$(dirname "$0")/../.." || exit 1
export ED_HOST=${ED_HOST:?} ED_USER=root
echo "build: $(bash logs/edremote.sh 'cat /proc/sys/kernel/tobyos_build' 2>/dev/null | tr -d '\r')"
bash logs/hwgate.sh > logs/hwchain-hwgate.log 2>&1
echo "hwgate: $(grep -a 'VERDICT' logs/hwchain-hwgate.log | tail -1)"
bash logs/hwtextgate.sh > logs/hwchain-text.log 2>&1
echo "$(grep -a 'HWTEXTGATE' logs/hwchain-text.log | tail -1)"
bash logs/hwsshchurn.sh > logs/hwchain-churn.log 2>&1
echo "$(grep -a 'HWSSHCHURN' logs/hwchain-churn.log | tail -1)"
ED_TIMEOUT=900 bash logs/hwoilspec.sh > logs/hwchain-oil.log 2>&1
echo "oilspec:"; grep -a -E "POSIX  |BASH-ONLY|^  [0-9]{4} |gained|LOST" logs/hwchain-oil.log | head -20
echo "== latency (fork/exit, signal delivery)"
bash tools/agent/hwlatency.sh > logs/hwchain-latency.log 2>&1
grep -a -E "^  (ok|FAIL)|HWLATENCY" logs/hwchain-latency.log
echo "== 841 KB uploads over SSH (finding 166)"
bash tools/agent/hwupstress.sh 40 > logs/hwchain-upstress.log 2>&1
grep -a -E "FAILED|UPSTRESS" logs/hwchain-upstress.log
echo "== kernel-shell probes"
KPORT=7777 bash logs/kprobechain.sh
echo "chain rc=$?"
