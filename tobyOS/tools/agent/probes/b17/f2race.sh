d=0; a=0
for i in 1 2 3 4 5 6 7 8 9 10; do
  sleep 3 & p=$!; kill -2 $p; sleep 0.2; if kill -0 $p 2>/dev/null; then a=$((a+1)); kill $p; else d=$((d+1)); fi; wait $p 2>/dev/null
done
echo "immediate: alive=$a dead=$d"
d=0; a=0
for i in 1 2 3 4 5; do
  sleep 3 & p=$!; sleep 0.2; kill -2 $p; sleep 0.2; if kill -0 $p 2>/dev/null; then a=$((a+1)); kill $p; else d=$((d+1)); fi; wait $p 2>/dev/null
done
echo "after 0.2s: alive=$a dead=$d"
