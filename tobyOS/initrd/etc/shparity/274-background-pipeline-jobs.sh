# A BACKGROUND PIPELINE IS A JOB OF ONE PROCESS AN ELEMENT (batch 14), as
# bash forks each element from the shell: `jobs -l` lists every process,
# `jobs -p` is the first one's pid (the group), `$!` the last one's; `wait`
# for any of them waits for the whole job and answers with its status;
# `kill %1` without job control signals each process; a script's report of
# a job killed by KILL is the long form, one line a process, and comes when
# the last process has gone. /bin/tsh ran the pipeline in ONE forked child:
# one pid, `$!` the child's, the job over when the child was. The inner
# script runs under the same shell, its pids (and their widths) masked.
exec 2>&1
cat > inner.sh <<'EOF'
t() { local e=$((SECONDS + $1)); while [ $SECONDS -lt $e ]; do :; done; }
sleep 2 | sleep 1 &
last=$!
jobs
jobs -l
[ "$(jobs -p)" != "$last" ] && echo "jobs -p is the first, \$! the last"
wait %1; echo "wait %1: $?"
true | (exit 3) &
wait $!; echo "wait \$!: $?"
(exit 4) | true &
wait %%; echo "wait %%: $?"
(sleep 1; exit 2) | (sleep 2; exit 3) &
first=$(jobs -p)
wait $first; echo "wait first: $?"
jobs
sleep 5 | sleep 5 &
t 1
kill %1; wait %1; echo "killed: $?"
sleep 5 | sleep 6 &
t 1
kill $!; t 1; jobs -l
wait %1; echo "waited: $?"
sleep 5 | sleep 5 &
t 1
kill -9 %1; t 1
echo "after the job is killed"
wait
sleep 5 | sleep 6 &
t 1
kill -9 $!; t 1
echo "after its last process is killed"
wait
echo "levels: $(echo $BASH_SUBSHELL | cat & wait) $({ echo $BASH_SUBSHELL; } | cat & wait)"
sleep 1 | sleep 1 &
jobs -r
disown %1
jobs
echo end
EOF
# pids where they stand: after "[N]+ " (jobs -l), at a line's start (the
# later processes), after "line N: " (a report); then the blanks squeezed
"$BASH" inner.sh 2>&1 |
    sed -E 's/^(\[[0-9]+\][-+ ]) +[0-9]+ /\1 PID /; s/^ +[0-9]+ /  PID /; s/(line [0-9]+: +)[0-9]+ /\1PID /' |
    tr -s ' '
