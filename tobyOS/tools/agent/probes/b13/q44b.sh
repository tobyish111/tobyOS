exec 2>&1 </dev/null
cd /tmp
f=/tmp/q44.$$.sh
w() { printf '%s\n' "$@" > $f; $SH $f 2>&1 | sed "s#$f#SCRIPT#; s/[0-9][0-9][0-9]*/N/g"; echo "--"; }
c() { $SH -c "$1" 2>&1 | sed "s/[0-9][0-9][0-9]*/N/g"; echo "-- c"; }
c 'sleep 0 & sleep 1; wait %1; echo $?'
c 'sleep 0 & sleep 1; jobs; echo after'
c 'sleep 0 & sleep 1; wait $!; echo $?'
c '(exit 4) & sleep 1; wait $!; echo $?'
c 'set -m; sleep 0 & sleep 1; wait %1; echo $?'
c 'set -m; (exit 4) & sleep 1; wait $!; echo $?'
c 'set -m; sleep 1 & wait $!; echo $?'
c 'set -m; sleep 1 & wait; echo $?'
c 'set -m; sleep 1 & wait -n; echo $?'
c 'set -m; sleep 0 & sleep 0 & sleep 1; echo after'
c 'set -m; sleep 0 | cat & sleep 1; echo after'
c 'set -m; { sleep 0; } & sleep 1; echo after'
c 'set -m; HOME=/tmp; mkdir -p /tmp/q44d; sleep 0 & cd /tmp/q44d; sleep 1; echo after'
c 'set -m; HOME=/tmp; sleep 30 & cd /; jobs; kill %1; wait; echo after'
c 'set -m; sleep 0 & sleep 1; set -m; echo after'
c 'set -m; sleep 0 & set +m; sleep 1; echo after'
w 'sleep 0 &' 'sleep 1' 'wait %1; echo $?'
w 'set -m' 'sleep 30 &' 'cd /' 'jobs' 'kill %1' 'wait' 'echo after'
w 'set -m' 'sleep 30 &' 'kill $!' 'wait $!' 'echo $?'
rm -rf $f /tmp/q44d
echo end
