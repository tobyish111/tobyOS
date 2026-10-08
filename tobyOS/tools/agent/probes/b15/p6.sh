# p6: INT to a script waiting for a foreground child; `( list ) &` as one
# process; read -t against signals; fatal signals in subshells.
exec 2>&1
echo "== i1: INT to the shell alone while it waits for a foreground child"
set -m
"$BASH" -c 'trap "echo i1-exit" EXIT; sleep 3; echo i1-after' & p=$!
set +m
sleep 1; s=$SECONDS; kill -INT $p; wait $p; echo "i1 st=$? waited=$((SECONDS-s))"
echo "== i2: the same with no EXIT trap"
set -m
"$BASH" -c 'sleep 3; echo i2-after' & p=$!
set +m
sleep 1; s=$SECONDS; kill -INT $p; wait $p; echo "i2 st=$? waited=$((SECONDS-s))"
echo "== i3: INT to the group -- the child dies of it"
set -m
"$BASH" -c 'trap "echo i3-exit" EXIT; sleep 3; echo i3-after' & p=$!
set +m
sleep 1; s=$SECONDS; kill -INT -$p; wait $p; echo "i3 st=$? waited=$((SECONDS-s))"
echo "== i4: INT to the group -- the child catches it"
set -m
"$BASH" -c 'trap "echo i4-exit" EXIT; "$BASH" -c "trap \"exit 0\" INT; sleep 3 & wait"; echo "i4-after $?"' & p=$!
set +m
sleep 1; kill -INT -$p; wait $p; echo "i4 st=$?"
echo "== j: ( list ) & is one process"
( echo "pid=$BASHPID" > /tmp/p6j1 ) & p=$!
wait
[ "$(cat /tmp/p6j1)" = "pid=$p" ] && echo "j1 same" || echo "j1 differs"
( echo "j2 $BASH_SUBSHELL" ) & wait
{ echo "j3 $BASH_SUBSHELL"; } & wait
( ( echo "j4 $BASH_SUBSHELL" ) ) & wait
( sleep 2; echo j5-orphan-ran ) & p=$!
sleep 0.3; kill $p; wait $p; echo "j5 st=$?"; sleep 2
echo "== r: read -t and signals"
s=$SECONDS; read -t 2 x < <(sleep 4); echo "r2 rc=$? waited=$((SECONDS-s))"
trap 'echo r3-trap' USR1
( sleep 1; kill -USR1 $$ ) &
s=$SECONDS; read -t 3 x < <(sleep 5); echo "r3 rc=$? waited=$((SECONDS-s))"
wait
trap 'echo r4-trap' USR1
( sleep 1; kill -USR1 $$ ) &
s=$SECONDS; read x < <(sleep 3; echo data); echo "r4 rc=$? x=$x waited=$((SECONDS-s))"
wait
trap - USR1
echo "== x: fatal signals in subshells"
"$BASH" -c 'trap "echo x1-exit" EXIT; ( kill -TERM $BASHPID; echo x1-sub-alive ); echo "x1 sub=$?"'; echo "x1 st=$?"
"$BASH" -c 'trap "echo x2-exit" EXIT; ( trap "echo x2-inner" EXIT; kill -TERM $BASHPID; echo x2-sub-alive ); echo "x2 sub=$?"'; echo "x2 st=$?"
"$BASH" -c 'trap "echo x3-exit" EXIT; x=$(trap "echo x3-cs" EXIT; kill -TERM $BASHPID; echo alive); echo "x3 [$x] $?"'; echo "x3 st=$?"
"$BASH" -c 'trap "echo x4-exit" EXIT; f() { kill -TERM $$; echo x4-in-f; }; f; echo x4-after'; echo "x4 st=$?"
"$BASH" -c 'trap "echo x5-exit" EXIT; for i in 1 2; do kill -TERM $$; echo "x5-loop $i"; done'; echo "x5 st=$?"
echo end
