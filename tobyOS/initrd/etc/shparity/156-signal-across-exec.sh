# A signal sent between fork and the new image used to be dropped by execve.
n=0; i=0; while [ $i -lt 50 ]; do sleep 3 & p=$!; kill -TERM $p; wait $p; [ $? = 143 ] && n=$((n+1)); i=$((i+1)); done; echo "term $n/50"
n=0; i=0; while [ $i -lt 20 ]; do sleep 3 & p=$!; kill -KILL $p; wait $p; [ $? = 137 ] && n=$((n+1)); i=$((i+1)); done; echo "kill $n/20"
trap "" USR1; sleep 3 & p=$!; kill -USR1 $p; sleep 0.2; kill -0 $p && echo "ignored stays ignored"; kill -TERM $p; wait $p; echo "w $?"; trap - USR1
echo done
