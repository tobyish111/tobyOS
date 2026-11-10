# In a script a background job that EXITED stays in the table across
# lines until `jobs` lists it or `wait` collects it; one a SIGNAL ended
# goes at the next line; after `kill $(jobs -p); wait` the next jobs are
# [1] and [2] again; `wait $pid` still has a deleted job's status.
# (batch 12; probe p90 in C:\t\b12)
exec 2>&1
sleep 0.1 &
sleep 0.4
jobs
echo "j=$?"
wait %1; echo "w1=$?"
sleep 0.1 &
p=$!
sleep 0.4
wait $p; echo "wp=$?"
sleep 0.1 & sleep 0.4; jobs; wait %%; echo "same-line w=$?"
sleep 3 & sleep 3 &
kill $(jobs -p); wait
sleep 1 & sleep 1 & jobs
wait
