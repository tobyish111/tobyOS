# a1.sh: the findings audit, jobs and signals -- 1 2 (an async command's
# INT and QUIT in a script), 3 (jobs in a script), 4 (a killed job reported
# at a later reap), 12 (a job's display text), 28 (set -m in a script
# reports a killed job). Pids differ: compare with normcmp.sh.
exec 2>&1
N() { sed 's/[0-9][0-9]*/N/g'; }
sleep 3 & p=$!; kill -3 $p; sleep 0.3; kill -0 $p 2>/dev/null && echo "f1 quit: alive" || echo "f1 quit: dead"; kill $p 2>/dev/null; wait $p 2>/dev/null
sleep 3 & p=$!; kill -2 $p; sleep 0.3; kill -0 $p 2>/dev/null && echo "f2 int: alive" || echo "f2 int: dead"; kill $p 2>/dev/null; wait $p 2>/dev/null
sleep 3 & p=$!; sleep 0.2; echo "f2b SigIgn=$(sed -n 's/^SigIgn:[[:space:]]*//p' /proc/$p/status)"; kill $p; wait $p 2>/dev/null
sleep 2 & sleep 0.2; jobs | N; jobs -l | N; kill %1; wait 2>/dev/null
sleep 5 & k=$!; kill -HUP $k; e=$((SECONDS+2)); while ((SECONDS<e)); do :; done; echo "f4 after loop"; sleep 0.1; echo "f4 after fg"
{ sleep 1; echo x >/dev/null; } & ( sleep 1 ) & f() { sleep 1; }; f & jobs | N; wait
set -m; sleep 30 & kill %1; sleep 0.5; echo "f28 after"; set +m
echo end
