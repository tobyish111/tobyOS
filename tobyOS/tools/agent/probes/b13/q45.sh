exec 2>&1 </dev/null
cd /tmp
f=/tmp/q45.$$.sh
w() { printf '%s\n' "$@" > $f; $SH --posix $f 2>&1 | sed "s#$f#SCRIPT#; s/[0-9][0-9][0-9]*/N/g"; echo "--"; }
c() { $SH --posix -c "$1" 2>&1 | sed "s/[0-9][0-9][0-9]*/N/g"; echo "-- c"; }
c 'sleep 30 & kill -9 $!; sleep 1; echo after'
c 'sleep 0 & sleep 1; wait %1; echo $?'
c 'sleep 0 & sleep 0 & sleep 1; wait %1; echo $?'
c 'sleep 0 & sleep 0 & sleep 1; jobs; echo after'
c 'set -m; sleep 0 & sleep 1; echo after'
c 'set -m; sleep 30 & kill $!; sleep 1; echo after'
c 'set -m; sleep 30 & kill -9 $!; sleep 1; echo after'
c 'set -m; sleep 1 & wait $!; echo $?'
w 'sleep 30 &' 'kill -9 $!' 'sleep 1' 'echo after'
w 'set -m' 'sleep 30 &' 'kill $!' 'sleep 1' 'echo after'
w 'set -m' 'sleep 0 &' 'sleep 1' 'jobs' 'echo after'
$SH -c 'x=$(sleep 0 & sleep 1; jobs; echo in); echo "[$x]"' 2>&1 | sed "s/[0-9][0-9][0-9]*/N/g"
$SH -c 'set -m; x=$(sleep 0 & sleep 1; echo in); echo "[$x]"' 2>&1 | sed "s/[0-9][0-9][0-9]*/N/g"
$SH -c 'set -m; (sleep 0 & sleep 1; echo in); echo after' 2>&1 | sed "s/[0-9][0-9][0-9]*/N/g"
$SH -c 'set -m; f() { sleep 0 & sleep 1; }; f | cat; echo after' 2>&1 | sed "s/[0-9][0-9][0-9]*/N/g"
rm -f $f
echo end
