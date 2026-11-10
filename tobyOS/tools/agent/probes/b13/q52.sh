exec 2>&1 </dev/null
for i in 1 2 3 4; do $SH -c 'set -m; echo "1 $-"; (echo "2 $-"); echo "3 $(echo $-)"; { echo "4 $-"; } | cat; (echo "5 $-") & wait; (set -m; echo "6 $-"); echo "7 $-"'; echo --; done
for i in 1 2 3; do $SH -c 'set -m; { echo 4; } | cat; (echo 5) & wait; echo 6'; echo --; done
for i in 1 2 3; do $SH -c 'set -m; (echo 5) & wait; (set -m; echo 6)'; echo --; done
echo end
