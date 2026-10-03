exec 2>&1
sleep 0.5 & kill -15 %%; echo "1 k=$?"; wait %%; echo "1 w=$?"
sleep 0.5 & kill -15 %%; sleep 0.2; wait %%; echo "2 w=$?"
sleep 0.2 & sleep 0.5; wait %%; echo "3 w=$?"
sleep 0.5 & pid=$!; kill -15 $pid; wait $pid; echo "4 w=$?"
sleep 0.5 & pid=$!; kill -15 %1; wait %1; echo "5 w=$?"; wait $pid; echo "5b w=$?"
sleep 0.5 & kill -9 %%; wait %%; echo "6 w=$?"
sleep 0.5 & kill -15 %%; jobs; echo "7"; wait; echo "7 w=$?"
sleep 0.3 & sleep 0.5; jobs; wait %1; echo "8 w=$?"
