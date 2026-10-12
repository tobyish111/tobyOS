#!/bin/bash
# forklat2.sh SHELL [N] -- Oils 2272's shape, `echo A & echo B` with the
# script's stdout a file, timed: SHELL runs `t0=$EPOCHREALTIME; echo c
# $EPOCHREALTIME & echo p $EPOCHREALTIME; wait; echo t $t0`, N times. Prints
# per run the child's and the parent's microseconds after t0 and which line
# the file has first. Run under bash.
SH=$1; N=${2:-10}
for i in $(seq 1 "$N"); do
  "$SH" -c 't0=$EPOCHREALTIME; echo c $EPOCHREALTIME & echo p $EPOCHREALTIME; wait; echo t $t0' > /tmp/fl2.out < /dev/null
  awk '{ s=$2; sub(/\./,"",s); v[$1]=s+0; if (NR==1) f=$1 }
       END { printf "child=%d parent=%d first=%s\n", v["c"]-v["t"], v["p"]-v["t"], f }' /tmp/fl2.out
done
