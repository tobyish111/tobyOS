exec 2>&1 </dev/null
$SH -c 'enable -a'
echo "== -n list"
$SH -c 'enable -n echo test; enable -n; enable -p | grep -- "-n"'
echo "== -f/-d"
$SH -c 'enable -f /x.so foo; echo "st=$?"'
$SH -c 'enable -d foo; echo "st=$?"'
$SH -c 'enable -d echo; echo "st=$?"'
$SH -c 'enable nosuch; echo "st=$?"'
$SH -c 'enable -s | head -3; echo "st=$?"'
$SH -c 'enable -x; echo "st=$?"'
echo end
