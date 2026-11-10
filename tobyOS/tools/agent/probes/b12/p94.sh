exec 2>&1
f() {
  sleep 0.2
  echo "in f"
}
sleep 1 & k=$!; kill -9 $k; f; echo "after f"
g() { sleep 0.2; echo "in g"; }
sleep 1 & k=$!; kill -9 $k
g
sleep 1 & k=$!; kill -9 $k; for i in 1; do
  sleep 0.2
done
echo "after for"
sleep 1 & k=$!; kill -9 $k; sleep 0.2 &
wait $!; echo "w=$?"
sleep 1 & k=$!; kill -9 $k; sleep 0.2 & wait; echo "plain wait"
sleep 1 & k=$!; kill -9 $k; eval 'sleep 0.2'; echo "after eval"
sleep 1 & k=$!; kill -9 $k; cat <(sleep 0.2; echo ps); echo "after ps"
echo end
