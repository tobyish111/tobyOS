# waitn.sh -- bash's OWN race in `wait -n` (finding 74; Oils 0463): the
# same lines, run 20 times each in a fresh shell, give bash different
# answers from run to run, so no single answer can be the oracle's. Each
# scenario prints its outcomes with their counts, for $SH.
#   w1  b14/r5's tail: after `kill %1; wait`, a new background pipeline
#       and `wait -n` -- 127 or 0 for bash
#   w2  Oils 0463: `echo x &`, `wait -n $!`, then `wait -n $! bad` --
#       whether the job is still there to be named by then
exec 2>&1 </dev/null
SH=${SH:-bash}
count() { sort | uniq -c | sed 's/^ *//'; }
echo "== w1"
i=0; while [ $i -lt 20 ]; do i=$((i+1))
  $SH -c 'set -m; sleep 5 | cat & sleep 0.3; kill %1 2>/dev/null; wait
          false | true | true & wait -n; echo "wait -n: $?"' 2>/dev/null
done | count
echo "== w2"
i=0; while [ $i -lt 20 ]; do i=$((i+1))
  ( cd /tmp && $SH -c 'echo x > /dev/null & wait -n $!; a=$?
          wait -n $! bad 2>/tmp/waitn.err; b=$?
          echo "first=$a second=$b errlines=$(wc -l < /tmp/waitn.err)"' )
done | count
rm -f /tmp/waitn.err
echo end
