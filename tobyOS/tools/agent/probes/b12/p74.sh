exec 2>&1
t=0.3
sleep $t &
jobs
{ sleep $t; echo   x; } >/dev/null &
jobs
for i in 1; do sleep $t; done &
jobs
wait
sleep 5 & k=$!
kill -9 $k; sleep 0.1
x=5; bash -c 'kill -9 $$' arg "$x"; echo "s=$?"
( kill -9 $BASHPID ); echo "s=$?"
{ bash -c 'kill -9 $$'; } ; echo "s=$?"
