exec 2>&1
trap 'echo "[D] $BASH_COMMAND"' DEBUG
echo hi >/dev/null
x=1   y=$x
echo   "a  b"   'c'   $x 2>&1
f() { echo "in f: $BASH_COMMAND"; }
f arg1
echo $(echo inner) | cat
true && false || echo or
[[ -n $x ]]
(( x + 1 ))
for i in 1; do echo loop$i; done
if true; then :; fi
cat <<EOF >/dev/null
body
EOF
echo a;echo b
trap - DEBUG
trap 'echo "[E] $BASH_COMMAND"' ERR
false
ls /nonexistent >/dev/null 2>&1
echo "last: $BASH_COMMAND"
