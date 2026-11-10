#!/bin/sh
# forklat.sh -- after `cmd &`, how long until the child and the parent each
# reach their next command? $EPOCHREALTIME is expanded in the background
# child's echo and in the parent's next echo; t0 just before the fork.
# Microseconds after t0, one line per run: child parent (and which wrote
# first, from the file's order).
us() { local s=${1%.*} f=${1#*.}; f=${f}000000; echo $(( (s % 1000) * 1000000 + 10#${f:0:6} )); }
for i in 1 2 3 4 5 6 7 8 9 10; do
  rm -f /tmp/fl.out
  t0=$EPOCHREALTIME; echo "c $EPOCHREALTIME" >> /tmp/fl.out & echo "p $EPOCHREALTIME" >> /tmp/fl.out
  wait
  b=$(us "$t0")
  set -- $(cat /tmp/fl.out)
  first=$1
  if [ "$1" = c ]; then c=$2 p=$4; else p=$2 c=$4; fi
  echo "child=$(( $(us "$c") - b )) parent=$(( $(us "$p") - b )) first=$first"
done
