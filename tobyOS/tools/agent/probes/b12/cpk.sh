exec 2>&1
coproc sleep 1
coproc cat 2>&1
echo "pid=$COPROC_PID"
kill $COPROC_PID; echo "kill=$?"
sleep 0.3
ps
jobs -l
