# Job specs by name and the jobs builtin's options and operands, as bash
# 5.2 has them: %name (a command's start), %?str (anywhere in it, case
# ignored), "ambiguous job spec", the `%` optional; jobs -l -p -n -r -s -x
# and named jobs. tsh knew %N %% %+ %- only, had `jobs -p` alone and listed
# every job whatever it was asked (logs/b13 q48 q49).
exec 2>&1
sleep 30 &
sleep 31 &
jobs %?30
jobs %sleep; echo "ambiguous=$?"
jobs %?3; echo "ambiguous2=$?"
jobs 1 %2 %5; echo "operands=$?"
jobs %-; jobs -; jobs %+
jobs -r
jobs -s; echo "stopped=$?"
jobs -l | sed 's/^\(\[[0-9]*\]. \) *[0-9][0-9]* /\1PID /'
jobs -lp | wc -l
jobs -x echo %1 %2 x | wc -w
jobs -lx echo; echo "lx=$?"
jobs -z; echo "z=$?"
jobs -x; echo "x-empty=$?"
sleep 32 &
jobs -n
echo "-- again"
jobs -n
kill %?30 %?31 %?32; echo "kill=$?"
kill %nope %?nope; echo "kill-bad=$?"
wait
jobs; echo "end=$?"
