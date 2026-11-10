exec 2>&1 </dev/null
# Background pipelines under set -m in a script: their notices, kills and
# waits (bash: a job of several processes in one group led by the first).
P() { sed -E 's/[0-9]{2,}/N/g'; }
t() { local e=$((SECONDS + $1)); while [ $SECONDS -lt $e ]; do :; done; }
set -m
sleep 2 | sleep 1 &
jobs -l | P
t 3
echo next
jobs -l | P
{ sleep 1; exit 4; } | cat &
t 2
jobs -l | P
echo x | { read v; exit 5; } &
t 1
echo after
sleep 5 | sleep 5 &
t 1
kill %1
t 1
echo "after kill"
sleep 5 | cat &
t 1
kill $!
t 1
echo "after kill \$!"
jobs -l | P
kill %1 2>/dev/null; wait
false | true | true &
wait -n; echo "wait -n: $?"
sleep 1 | (exit 6) &
wait -n -p v; echo "wait -n -p: $? $([ "$v" = "$!" ] && echo last-pid || echo other)"
echo end
