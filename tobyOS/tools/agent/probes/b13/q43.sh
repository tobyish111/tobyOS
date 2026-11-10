exec 2>&1 </dev/null
cd /tmp
f=/tmp/q43.$$.sh
w() { printf '%s\n' "$@" > $f; $SH $f 2>&1 | sed "s#$f#SCRIPT#; s/[0-9][0-9][0-9]*/N/g"; echo "--"; }
c() { $SH -c "$1" 2>&1 | sed "s/[0-9][0-9][0-9]*/N/g"; echo "-- c"; }
c 'set -m; sleep 0 & sleep 1; echo after'
c 'set -m; sleep 30 & kill $!; sleep 1; echo after'
c 'set -m; (exit 3) & sleep 1; echo after'
c 'sleep 0 & sleep 1; echo after'
c 'sleep 30 & kill $!; sleep 1; echo after'
c 'set -m; sleep 0 & cd /; sleep 1; echo after'
c 'set -m; trap : USR1; sleep 30 & kill -USR1 $!; sleep 1; echo after'
c 'set -m; sleep 0 &
sleep 1
echo after'
c 'set -m; sleep 0 & sleep 1; jobs; echo after'
c 'set -m; sleep 0 & sleep 1; wait; echo after'
c 'set -m; f() { sleep 0 & sleep 1; echo in; }; f; echo after'
c 'set -m; sleep 0 & x=$(sleep 1); echo after'
c 'set -m; sleep 0 & sleep 1 | cat; echo after'
c 'set -m; sleep 0 & sleep 1; (echo sub); echo after'
c 'set -m; sleep 0 & sleep 1; set +m; echo after'
c 'set -m; sleep 30 & kill -STOP $!; sleep 1; echo after; kill -9 %1'
w 'set -m' 'sleep 30 &' 'kill -USR1 $!' 'sleep 1' 'echo after'
w 'set -m' 'trap : USR1' 'sleep 30 &' 'kill -USR1 $!' 'sleep 1' 'echo after'
w 'set -m' 'sleep 30 &' 'kill $!' 'cd /' 'sleep 1' 'echo after'
w 'set -m' 'sleep 30 &' 'kill $!' 'f() { echo in; }' 'f' 'echo after'
w 'set -m' 'sleep 30 & kill $!; sleep 1; echo same'
w 'set -m' 'sleep 30 &' 'kill -PIPE $!' 'sleep 1' 'echo after'
w 'set -m' 'sleep 30 &' 'kill -HUP $!' 'sleep 1' 'echo after'
rm -f $f
echo end
