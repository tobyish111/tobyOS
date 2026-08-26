set -o errexit

pids=''
for i in 3 2 1; do
  { sleep 0.0$i; echo $i; exit $i; } &
  pids="$pids $!"
done

for pid in $pids; do
  set +o errexit
  wait $pid
  status=$?
  set -o errexit

  echo status=$status
done
