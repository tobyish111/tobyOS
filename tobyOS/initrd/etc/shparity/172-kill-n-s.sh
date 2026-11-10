# kill -n and kill -s each take a signal NAME or NUMBER (S14, Oils 0931).
# tsh had only -s, and only by name: `kill -n 9 PID` and `kill -s 15 PID`
# were "unknown signal". A KILL report names a pid, so it is kept out.
sleep 5 & k=$!; kill -n 9 $k; echo "kill -n 9: $?"; wait $k 2>/dev/null; echo "killed: $?"
sleep 5 & k=$!; kill -s 15 $k; echo "kill -s 15: $?"; wait $k; echo "termed: $?"
sleep 5 & k=$!; kill -n TERM $k; echo "kill -n TERM: $?"; wait $k; echo "termed: $?"
sleep 5 & k=$!; kill -s SIGTERM $k; echo "kill -s SIGTERM: $?"; wait $k; echo "termed: $?"
kill -n 99 $$ 2>/dev/null; echo "kill -n 99: $?"
kill -s NOSUCH $$ 2>/dev/null; echo "kill -s NOSUCH: $?"
kill -n 2>/dev/null; echo "kill -n alone: $?"
kill -n 0 $$; echo "kill -n 0: $?"
echo done
