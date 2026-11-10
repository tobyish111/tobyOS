# wait -n / -p / -f (S9). tsh had none of them:
# `wait -n` was "invalid option", status 2.
(sleep 0.4; exit 4) & (sleep 0.1; exit 5) &
wait -n; echo "wait -n, first to finish: $?"
wait -n; echo "wait -n, the other: $?"
wait -n; echo "wait -n, no jobs left: $?"
(sleep 0.1; exit 6) & p=$!
wait -n -p v; echo "wait -n -p: $? [$([ "$v" = "$p" ] && echo the-pid)]"
(sleep 0.3; exit 7) & q=$!; (sleep 0.05; exit 8) &
wait -n $q; echo "wait -n listed: $?"
wait
(exit 9) & sleep 0.3
wait -n; echo "wait -n, finished before it was asked: $?"
(sleep 0.1; exit 3) & r=$!
wait -f $r; echo "wait -f: $?"
echo x & wait -n $! nosuchjob 2>/dev/null; echo "wait -n with a bad id: $?"
wait -n nosuchjob 2>/dev/null; echo "wait -n only a bad id: $?"
wait -p 2>/dev/null; echo "wait -p without a name: $?"
wait -p 1bad 2>/dev/null; echo "wait -p bad name: $?"
v=keep; wait -p v; echo "wait -p, no jobs: $? [${v-unset}]"
# The bookkeeping those answers rest on (bash 5.2): finished jobs and the
# statuses of reaped ones are kept until something collects them.
(exit 3) & wait $! $! $!; echo "the same pid three times: $?"
(exit 4) & p=$!; wait -n; wait $p; echo "a pid wait -n collected: $?"
(exit 5) & p=$!; sleep 0.2; wait; wait $p 2>/dev/null; echo "after a plain wait: $?"
(exit 6) & sleep 0.2; wait; (exit 7) & sleep 0.2; wait -n; echo "kept by a plain wait: $?"; wait -n; echo "then: $?"
(exit 10) & sleep 0.2; (exit 11) & sleep 0.2; wait; wait -n; echo "only \$! is kept: $?"; wait -n; echo "then: $?"
sleep 0.2 & wait; wait -n; echo "a job wait waited for is gone: $?"
wait 99999 2>/dev/null; echo "not a child: $?"
wait %9 2>/dev/null; echo "no such job: $?"
wait bad 2>/dev/null; echo "not a pid: $?"
wait -- 99999 2>/dev/null; echo "after --: $?"
# bash's mark on a FINISHED job is "+" or, under load, " " (NEW-FINDINGS 19),
# so it is blanked; `jobs` writes to a file, not a pipe, to run in THIS shell.
false & sleep 0.2; jobs > /tmp/fx167.j; sed 's/^\(\[[0-9]*\]\)[-+ ]/\1 /' /tmp/fx167.j; rm -f /tmp/fx167.j; wait -n; echo "after jobs listed it: $?"
# A job a SIGNAL killed is dropped as soon as bash notices; its status stays.
sleep 5 & k=$!; kill $k; sleep 0.2; wait -n; echo "wait -n after a killed job: $?"
sleep 5 & k=$!; kill $k; sleep 0.2; wait $k; echo "wait PID of a killed job: $?"
sleep 5 & sleep 5 & kill $(jobs -p); sleep 0.2; wait; sleep 1 & jobs; kill %%; wait
sleep 1 & jobs | wc -l; kill %%; wait
echo done
