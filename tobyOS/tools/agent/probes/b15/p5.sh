# p5: a trapped signal against wait, a foreground child and read; and
# SIGQUIT, which every bash ignores (sig.c initialize_shell_signals).
exec 2>&1
echo "== w1: wait PID and a trapped signal"
trap 'echo w1-trap' USR1
sleep 3 & p=$!
( sleep 1; kill -USR1 $$ ) &
s=$SECONDS; wait $p; echo "w1 rc=$? waited=$((SECONDS-s))"
wait
echo "== w2: bare wait and a trapped signal"
trap 'echo w2-trap' USR1
sleep 3 &
( sleep 1; kill -USR1 $$ ) &
s=$SECONDS; wait; echo "w2 rc=$? waited=$((SECONDS-s))"
wait
echo "== w3: a foreground child and a trapped signal"
trap 'echo w3-trap' USR1
( sleep 1; kill -USR1 $$ ) &
s=$SECONDS; sleep 3; echo "w3 rc=$? waited=$((SECONDS-s))"
wait
echo "== r1: read and a trapped signal"
trap 'echo r1-trap' USR1
( sleep 1; kill -USR1 $$ ) &
s=$SECONDS; read -t 3 x < <(sleep 4); echo "r1 rc=$? waited=$((SECONDS-s))"
wait
trap - USR1
echo "== q: SIGQUIT"
"$BASH" -c 'kill -QUIT $$; echo q1-alive'; echo "q1 st=$?"
"$BASH" -c 'sed -n "s/^SigIgn:[[:space:]]*/q2 /p" /proc/self/status; kill -QUIT $$; echo q2-alive'; echo "q2 st=$?"
"$BASH" -c '( kill -QUIT $BASHPID; echo q3-sub-alive ); echo "q3 sub=$?"'; echo "q3 st=$?"
"$BASH" -c 'x=$(kill -QUIT $BASHPID; echo q4-cs-alive); echo "$x"'; echo "q4 st=$?"
"$BASH" -c 'trap "echo q5-trap" QUIT; kill -QUIT $$; echo q5-alive; trap - QUIT; kill -QUIT $$; echo q5-still'; echo "q5 st=$?"
echo end
