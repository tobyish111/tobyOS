# k3-bash.sh -- run under the guest's GNU bash, 2>&1: does bash see a signal
# death as one? Linux bash: $? 137/143 either way, but "Killed"/"Terminated"
# reports appear only when wait4 says WIFSIGNALED.
sleep 5 & p=$!; kill -9 $p; wait $p; echo "bg kill -9 -> \$?=$?"
sleep 5 & p=$!; kill -TERM $p; wait $p; echo "bg kill -TERM -> \$?=$?"
bash -c 'kill -9 $$'; echo "fg bash -c 'kill -9 \$\$' -> \$?=$?"
bash -c 'exit 137'; echo "fg exit 137 -> \$?=$?"
echo done
