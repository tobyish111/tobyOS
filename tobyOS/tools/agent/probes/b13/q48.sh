exec 2>&1 </dev/null
cd /tmp
f=/tmp/q48.$$.sh
w() { printf '%s\n' "$@" > $f; $SH $f 2>&1 | sed "s#$f#SCRIPT#; s/[0-9][0-9][0-9]*/N/g"; echo "--"; }
w 'sleep 30 &' 'kill %sleep; echo $?' 'wait'
w 'sleep 30 &' 'sleep 31 &' 'kill %sleep; echo $?' 'kill %1 %2' 'wait'
w 'sleep 30 &' 'kill %?30; echo $?' 'wait'
w 'sleep 30 &' 'sleep 31 &' 'kill %?3; echo $?' 'kill %1 %2' 'wait'
w 'sleep 30 &' 'kill %nope; echo $?' 'kill %?nope; echo $?' 'kill %1; wait'
w 'sleep 30 &' 'jobs %sleep; jobs %?30; echo $?' 'kill %1; wait'
w 'sleep 30 &' 'sleep 31 &' 'jobs %?3; echo $?' 'jobs %sl; echo $?' 'kill %1 %2; wait'
w 'sleep 30 &' 'jobs 1; echo $?' 'kill 1 2>/dev/null; echo $?' 'kill %1; wait'
w 'sleep 30 &' 'disown %sleep; echo $?' 'jobs; echo $?' 'kill $!'
w 'sleep 30 &' 'wait %?nope; echo $?' 'kill %1; wait %sleep; echo $?'
w 'sleep 30 &' 'jobs -p %sleep' 'kill %1; wait'
w 'set -m' 'sleep 1 &' 'fg %sleep; echo $?'
w 'set -m' 'sleep 1 &' 'fg 1; echo $?'
w 'set -m' 'sleep 1 &' 'fg %?eep; echo $?'
w 'sleep 30 &' 'set -m' 'bg %1; echo $?' 'fg %1; echo $?' 'kill %1; wait'
w 'set -m' 'sleep 30 &' 'fg -x; echo $?' 'bg -x; echo $?' 'kill %1; wait'
w 'set -m' 'fg; echo $?' 'bg; echo $?' 'fg %3; echo $?' 'bg 3; echo $?' 'bg %3 %4; echo $?'
w 'set -m' 'sleep 30 &' 'bg %1 %2; echo $?' 'kill %1; wait'
w 'set -m' 'sleep 30 &' 'bg; echo "$!" | grep -c .' 'kill %1; wait'
rm -f $f
echo end
