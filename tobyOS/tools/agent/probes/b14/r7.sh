exec 2>&1 </dev/null
# The report of a background job a signal killed, in a script: one
# process, a pipeline, by TERM and by KILL; and where an async command's
# stdin is /dev/null and where it is not.
P() { sed -E 's/[0-9]{2,}/N/g'; }
t() { local e=$((SECONDS + $1)); while [ $SECONDS -lt $e ]; do :; done; }
sleep 5 &
t 1; kill %1; t 1
echo "after one TERM"
wait
sleep 5 | sleep 5 &
t 1; kill %1; t 1
echo "after pipeline TERM"
wait
sleep 5 | sleep 5 &
t 1; kill -9 %1; t 1
echo "after pipeline KILL"
wait
sleep 5 &
t 1; kill -9 %1; t 1
echo "after one KILL"
wait
sleep 5 | sleep 6 &
t 1; kill -9 $!; t 1
echo "after last KILL"
wait
echo "== stdin"
printf 'L\n' | $SH -c 'while false; do :; done < /dev/null; cat & wait; echo e1'
printf 'L\n' | $SH -c '{ cat & wait; } 0<&0; echo e2'
printf 'L\n' | $SH -c 'echo x | { cat & wait; }; echo e3'
printf 'L\n' | $SH -c '( cat & wait ); echo e4'
printf 'L\n' | $SH -c 'f() { cat & wait; }; f; echo e5'
printf 'L\n' | $SH -c 'cat 0<&0 & wait; echo e6'
printf 'L\n' | $SH -c 'x=$(cat & wait); echo "e7 [$x]"'
echo end
