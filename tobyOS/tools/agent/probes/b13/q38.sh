exec 2>&1 </dev/null
cd /tmp
printf '%s\n' 'set -o history' 'echo one two' "history -p '!!' '!\$' '^one^1^' 'x!' '!nosuch' 'plain'" 'echo "st=$?"' "history -p '!!:p'" 'echo "st=$?"' 'history' > /tmp/q38a.sh
$SH /tmp/q38a.sh; echo "rc=$?"
echo "== enable"
$SH -c 'enable -n echo; enable -p | grep -c "enable -n echo"; enable echo; type -t echo'
$SH -r -c 'enable -n echo; echo "st=$?"; type -t echo'
$SH -r -c 'enable -n echo 2>/dev/null; enable echo; echo "st=$?"; type -t echo'
$SH -r -c 'enable -f /x.so foo; echo "st=$?"'
$SH -r -c 'enable -d foo; echo "st=$?"'
$SH -r -c 'enable; echo "st=$?"' | tail -2
$SH -r -c 'enable -a | head -2; echo "st=$?"'
echo end
