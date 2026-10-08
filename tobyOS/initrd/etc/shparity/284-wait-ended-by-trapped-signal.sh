# `wait` IS ENDED BY A TRAPPED SIGNAL (POSIX 2.11): it returns at once,
# 128 + the signal, and then the trap runs -- for `wait PID` and a bare
# `wait` alike. A foreground command is not interrupted: the trap waits for
# it. tsh's wait sat until the job was done and said 0 (logs/b15 p5).
exec 2>&1
trap 'echo w1-trap' USR1
sleep 3 & p=$!
( sleep 1; kill -USR1 $$ ) &
wait $p; echo "w1 $?"
wait
trap 'echo w2-trap' USR1
sleep 3 &
( sleep 1; kill -USR1 $$ ) &
wait; echo "w2 $?"
wait
trap 'echo w3-trap' USR1
( sleep 1; kill -USR1 $$ ) &
sleep 2; echo "w3 $?"
wait
trap - USR1
"$BASH" -c 'trap "echo w4-trap; exit 5" USR1; ( sleep 1; kill -USR1 $$ ) & sleep 3 & wait $!'; echo "w4 $?"
echo end
