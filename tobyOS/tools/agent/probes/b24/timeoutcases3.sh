#!/bin/sh
# timeoutcases3.sh -- timeout's getopt errors, long-name prefixes and the
# real-time signal range: the status after each, stderr as it comes.
#
#     sh timeoutcases3.sh [TIMEOUT]
T=${1:-timeout}
"$T" --v 1 true; echo "st=$?"
"$T" -s; echo "st=$?"
"$T" --signal; echo "st=$?"
"$T" --verbose=x 1 true; echo "st=$?"
"$T" -k; echo "st=$?"
"$T" --kill-after=0.1 0.2 true; echo "st=$?"
"$T" --p 0.2 sleep 1; echo "st=$?"
"$T" --f 0.2 sleep 1; echo "st=$?"
"$T" -s 64 0.2 sleep 1; echo "st64=$?"
"$T" -s 65 0.2 sleep 1; echo "st65=$?"
"$T" -s RTMIN 0.2 sleep 1; echo "rtmin=$?"
"$T" -- 0.2 sleep 1; echo "dd=$?"
