# S9: are jobs killed by a signal kept like exited ones? (bash 5.2)
sleep 5 & k=$!; kill $k; sleep 0.2; wait -n; echo "wait -n after a TERM'd job: $?"
sleep 5 & k=$!; kill $k; sleep 0.2; wait; sleep 1 & jobs; echo "a TERM'd \$!, after wait: $?"; kill %%; wait
sleep 5 & k=$!; kill -9 $k; sleep 0.2; wait 2>/dev/null; sleep 1 & jobs; echo "a KILLed \$!, after wait: $?"; kill %%; wait
(exit 3) & sleep 0.2; wait; sleep 1 & jobs; echo "an exited \$!, after wait: $?"; kill %%; wait; wait -n
sleep 5 & k=$!; kill $k; sleep 0.2; wait $k; echo "wait PID of a TERM'd job: $?"
sleep 5 & k=$!; kill $k; sleep 0.2; jobs; echo "jobs shows it: $?"
echo done
