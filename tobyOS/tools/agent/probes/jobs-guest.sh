# jobs format in a script, bash 5.2 on the guest (S6)
sleep 3 & sleep 3 &
x=1 sleep 3 &
{ sleep 3; } &
jobs
echo "--- -p $(jobs -p | wc -l)"
kill $(jobs -p) 2>/dev/null; wait
for i in $(seq 12); do sleep 3 & done
echo "jobs $(jobs | wc -l) pids $(jobs -p | wc -l)"
jobs | head -3; jobs | tail -2
kill $(jobs -p); wait
echo done
