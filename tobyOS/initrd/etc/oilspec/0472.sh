echo hi | { exit 99; } &
echo status=$?
wait $!
echo status=$?
echo --

pids=''
for i in 3 2 1; do
  sleep 0.0$i | echo i=$i | ( exit $i ) &
  pids="$pids $!"
done
#echo "PIDS $pids"

for pid in $pids; do
  wait $pid
  echo status=$?
done

# Not cleaned up
if false; then
  echo 'DEBUG'
  jobs --debug
fi
