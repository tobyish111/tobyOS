# wait4(-1) used to wait for the LOWEST-NUMBERED live child, not the first
# to change state: bash, holding `sleep 3 &`, got its foreground command
# back only after the sleep had exited -- and reaped the sleep in its place.
sleep 3 & p=$!
/bin/true
kill -0 $p && echo "a background job outlives a foreground command"
sleep 0.2 & q=$!
wait $q; echo "wait for the newer job: $?"
kill -0 $p && echo "and outlives a wait for another job"
kill -TERM $p; wait $p; echo "the first job: $?"
echo done
