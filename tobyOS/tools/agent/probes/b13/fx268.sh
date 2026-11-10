# fg, bg and disown as bash's: no job control is an error; a job started
# without it is refused under set -m later; a finished one "has
# terminated"; bg of a running job is "already in background", and
# success; options are usage errors; bg takes several jobs; a `%` command
# is fg; disown -a -r -h, pids, several operands. And a job's directory:
# "(wd: DIR)" in `jobs`, "<TAB>(wd: DIR)" from fg. tsh printed a kernel
# console line for fg, said "[N]+" of every bg, continued a running job and
# stopped at disown's first bad operand (logs/b13 q47-q49).
exec 2>&1
cd /tmp
fg; echo "fg-nojc=$?"
bg %1; echo "bg-nojc=$?"
sleep 30 &
set -m
bg %1; echo "bg-nonjc=$?"
fg %1; echo "fg-nonjc=$?"
kill %1; wait
sleep 0 &
sleep 1
fg %1; echo "fg-dead=$?"
bg %1; echo "bg-dead=$?"
wait
sleep 30 &
sleep 31 &
bg %1; echo "bg-running=$?"
bg %1 %2 %7; echo "bg-several=$?"
bg -x; echo "bg-opt=$?"
fg -x; echo "fg-opt=$?"
cd /
jobs
kill %1 %2; sleep 1
sleep 1 &
fg %1; echo "fg=$?"
sleep 1 &
%sleep; echo "percent=$?"
sleep 1 &
%%; echo "percent2=$?"
sleep 30 &
p1=$!
sleep 31 &
p2=$!
disown -r; echo "disown-r=$?"
jobs; echo "after-r=$?"
kill $p1 $p2
sleep 30 &
p3=$!
disown -h %1; echo "disown-h=$?"
jobs
disown %7 %1; echo "disown-two=$?"
jobs; echo "after=$?"
kill $p3
disown 99999; echo "disown-pid=$?"
disown -z; echo "disown-opt=$?"
disown; echo "disown-none=$?"
disown -a; echo "disown-a=$?"
echo end
