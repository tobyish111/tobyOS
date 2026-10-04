# Notices under `set -m` in a script, as bash 5.2 writes them: a job that
# a signal other than INT, TERM or PIPE ended is the "line N: PID Killed"
# report (not shown here: it has a pid); TERM and INT, or a trapped
# signal, get the standard notice "[N]+  Terminated  CMD" -- with
# "  (wd: DIR)" and "(wd now: DIR)" when the shell has moved -- and a job
# that exited is kept, quietly, for `jobs` or `wait`. `wait` for a job a
# signal ended reports it. tsh printed none of the standard notices and
# kept no directories (logs/b13 q42-q46).
exec 2>&1
cd /tmp
set -m
sleep 30 &
kill $!
sleep 1
echo one
sleep 30 &
kill -INT $!
sleep 1
echo two
trap : USR1
sleep 30 &
# not before the child has execed: until then it still has the trap
sleep 1
kill -USR1 $!
sleep 1
echo three
trap - USR1
sleep 30 &
cd /
kill %1
sleep 1
echo four
sleep 0 &
sleep 1
echo five
jobs
sleep 30 &
kill $!
wait $!
echo "six=$?"
(sleep 0 & sleep 1; echo in-subshell)
(set -m; sleep 0 & sleep 1; echo set-m-subshell)
echo end
