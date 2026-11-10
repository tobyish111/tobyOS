exec 2>&1 </dev/null
$SH -c 'set -m; echo "1 $-"; (echo "2 $-"); echo "3 $(echo $-)"; { echo "4 $-"; } | cat; (echo "5 $-") & wait; (set -m; echo "6 $-"); echo "7 $-"'
$SH -c 'set -m; (set -m; sleep 0 & sleep 1; echo in); echo after' 2>&1 | sed "s/[0-9][0-9][0-9]*/N/g"
$SH -c 'set -m; (sleep 0 & sleep 1; jobs; echo in); echo after' 2>&1 | sed "s/[0-9][0-9][0-9]*/N/g"
echo end
