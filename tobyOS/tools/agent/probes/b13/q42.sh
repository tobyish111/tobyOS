exec 2>&1 </dev/null
cd /tmp
f=/tmp/q42.$$.sh
w() { printf '%s\n' "$@" > $f; $SH $f 2>&1 | sed "s#$f#SCRIPT#; s/[0-9][0-9]*/N/g"; echo "--"; }
w 'set -m' 'sleep 0 &' 'sleep 1' 'echo after'
w 'set -m' 'sleep 30 &' 'kill $!' 'sleep 1' 'echo after'
w 'set -m' 'sleep 30 &' 'kill $!' 'wait $!' 'echo after'
w 'set -m' 'sleep 30 &' 'kill -9 $!' 'sleep 1' 'echo after'
w 'set -m' 'sleep 30 &' 'kill -INT $!' 'sleep 1' 'echo after'
w 'set -m' '(exit 3) &' 'sleep 1' 'echo after'
w 'set -m' 'sleep 0 &' 'wait' 'echo after'
w 'set -m' 'sleep 0 & sleep 0 &' 'sleep 1' 'jobs' 'echo after'
w 'sleep 30 &' 'kill $!' 'sleep 1' 'echo after'
w 'sleep 30 &' 'kill -9 $!' 'sleep 1' 'echo after'
w 'sleep 30 &' 'kill -9 $!' 'wait $!' 'echo after'
rm -f $f
echo end
