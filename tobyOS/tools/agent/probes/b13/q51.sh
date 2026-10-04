exec 2>&1 </dev/null
for i in 1 2 3; do $SH -c 'set -m; (echo 5) & wait; echo 6'; echo --; done
for i in 1 2 3; do $SH -c 'set -m; (echo 5) & sleep 1; wait; echo 6'; echo --; done
for i in 1 2; do $SH -c 'set -m; (echo 5; sleep 1) & wait; echo 6'; echo --; done
for i in 1 2; do $SH -c 'set -m; (echo 5) & wait; (echo 6)'; echo --; done
echo end
