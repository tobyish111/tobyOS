exec 2>&1
busy() { e=$((SECONDS+2)); while ((SECONDS<e)); do :; done; }
sleep 1 & kill -9 $!; busy; jobs; echo "a after jobs"
echo "a next"
sleep 1 & kill -9 $!; busy; wait $!; echo "b w=$?"
echo "b next"
sleep 1 & kill -9 $!; busy; wait; echo "c w=$?"
echo "c next"
sleep 1 & kill -9 $!; busy; jobs -p >/dev/null; echo "d after jobs -p"
echo "d next"
sleep 1 & kill -9 $!; busy; sleep 0 & echo "e started"; wait
echo "e next"
sleep 1 & kill -15 $!; busy; jobs; echo "f after jobs"
echo "f next"
sleep 0.1 & busy; sleep 2 & jobs; kill %2; wait
echo "g next"
echo end
