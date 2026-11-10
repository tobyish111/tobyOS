exec 2>&1 </dev/null
$SH -r -c 'enable -n echo; enable echo; echo "st=$?"; type -t echo'
$SH -r -c 'enable -n echo test; enable -a echo test; echo "st=$?"'
$SH -r -c 'enable -n echo; enable -n echo; echo "st=$?"'
$SH -r -c 'enable -p | head -2; echo "st=$?"'
$SH -c 'enable -n echo; enable -a | grep -e "echo" -e " test"; enable -ps; echo "st=$?"' | head -20
$SH -c 'enable -ns; echo st=$?' | head -3
echo end
