exec 2>&1 </dev/null
cd /tmp
f=/tmp/q49.$$.sh
P='s/^(\[[0-9]+\]. )[ 0-9]{4}[0-9]/\1PPPPP/'
w() { printf '%s\n' "$@" > $f; $SH $f 2>&1 | sed -E "$P" | sed "s#$f#SCRIPT#; s/[0-9][0-9][0-9]*/N/g"; echo "--"; }
x() { printf '%s\n' "$@" > $f; $SH --posix $f 2>&1 | sed -E "$P" | sed "s#$f#SCRIPT#; s/[0-9][0-9][0-9]*/N/g"; echo "-- posix"; }
w 'set -m' 'sleep 1 &' '%1; echo $?'
w 'set -m' 'sleep 1 &' '%sleep; echo $?'
w 'set -m' 'sleep 1 &' '%%; echo $?'
w 'set -m' 'sleep 1 &' '%; echo $?'
w 'sleep 1 &' '%1; echo $?' 'wait'
w 'set -m' 'sleep 30 &' '%1 &' 'echo $?' 'kill %1' 'wait'
w 'set -m' 'sleep 1 &' '%1 extra; echo $?'
w 'set -m' '%5; echo $?'
w 'set -m' 'sleep 1 &' '"%1"; echo $?'
w 'set -m' 'sleep 1 &' 'x=%1; $x; echo $?'
w 'sleep 30 &' 'jobs -l' 'jobs -p | grep -c .' 'kill %1' 'wait'
w 'sleep 0 &' 'sleep 1' 'jobs -l' 'echo $?'
w '(exit 3) &' 'sleep 1' 'jobs -l'
w 'sleep 30 &' 'jobs -n' 'echo next' 'jobs -n' 'echo last' 'kill %1' 'wait'
w 'sleep 30 &' 'sleep 0 &' 'sleep 1' 'jobs -r' 'echo s' 'jobs -s' 'echo all' 'jobs' 'kill %1' 'wait'
w 'sleep 30 &' 'jobs -x echo %1 x %2 %% | sed "s/[0-9][0-9]*/N/"' 'echo $?' 'kill %1' 'wait'
w 'sleep 30 &' 'jobs -l -x echo %1; echo $?' 'jobs -lx echo; echo $?' 'kill %1' 'wait'
w 'sleep 30 &' 'jobs -z; echo $?' 'jobs %5; echo $?' 'jobs %1 %5 %1; echo $?' 'kill %1' 'wait'
w 'sleep 30 &' 'jobs -x false; echo $?' 'jobs -x; echo $?' 'kill %1' 'wait'
w 'sleep 30 &' 'jobs -lp' 'jobs -pl' 'kill %1' 'wait'
x '(exit 3) &' 'sleep 1' 'jobs' 'jobs -l'
x 'sleep 30 &' 'set -m' 'sleep 31 &' 'bg %2' 'kill %1 %2' 'wait'
w 'sleep 30 &' 'sleep 31 &' 'disown -r; echo $?' 'jobs; echo $?' 'kill $!'
w 'sleep 30 &' 'sleep 0 &' 'sleep 1' 'disown -r; echo $?' 'jobs' 'kill %1' 'wait'
w 'sleep 30 &' 'disown -h %1; echo $?' 'jobs' 'kill %1; wait'
w 'sleep 30 &' 'disown $!; echo $?' 'jobs; echo $?' 'kill $!'
w 'sleep 30 &' 'disown %5 %1; echo $?' 'jobs; echo $?' 'kill $!'
w 'sleep 30 &' 'disown -z; echo $?' 'disown 99999; echo $?' 'kill %1; wait'
w 'disown; echo $?' 'disown -a; echo $?' 'disown -r; echo $?'
rm -f $f
echo end
