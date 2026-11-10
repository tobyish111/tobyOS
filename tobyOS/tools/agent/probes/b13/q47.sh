exec 2>&1 </dev/null
cd /tmp
f=/tmp/q47.$$.sh
w() { printf '%s\n' "$@" > $f; $SH $f 2>&1 | sed "s#$f#SCRIPT#; s/[0-9][0-9][0-9]*/N/g"; echo "--"; }
c() { $SH -c "$1" 2>&1 | sed "s/[0-9][0-9][0-9]*/N/g"; echo "-- c"; }
c 'set -m; sleep 30 & bg %1; echo $?; kill %1'
c 'set -m; sleep 30 & sleep 31 & bg %1; bg; echo $?; kill %1 %2'
w 'set -m' 'sleep 0 &' 'sleep 1' 'fg %1; echo $?'
w 'set -m' 'sleep 0 &' 'sleep 1' 'bg %1; echo $?'
w 'sleep 30 &' 'fg %1; echo $?' 'kill %1'
w 'sleep 30 &' 'bg %1; echo $?' 'kill %1'
w 'set -m' 'sleep 1 &' 'cd /' 'fg %1; echo $?'
w 'set -m' 'sleep 1 &' 'cd /' 'fg; echo $?'
w 'set -m' 'HOME=/tmp' 'sleep 30 &' 'cd /' 'bg %1; echo $?' 'jobs' 'kill %1'
w 'set -m' 'sleep 30 &' 'set -o posix' 'bg %1' 'kill %1'
w 'set -m' 'sleep 1 | cat &' 'cd /' 'fg; echo $?'
rm -f $f
echo end
