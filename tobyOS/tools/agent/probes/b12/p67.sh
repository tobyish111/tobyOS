exec 2>&1
sleep 0.3 &
x=$(jobs); echo "[c-jobs:${x:+some}]"
( jobs | wc -l | tr -d ' ' ) | sed 's/^/s-jobs:/'
x=$(wait; echo "w=$?"); echo "[$x]"
( wait %1; echo "sw=$?" )
sleep 0.05 & k=$!
kill -9 $k
x=$(sleep 0.1; echo in); echo "[$x]"
( sleep 0.1; echo sin )
wait
echo end
