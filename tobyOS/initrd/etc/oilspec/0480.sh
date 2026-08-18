for i in 1 2 3; do
  echo $i
  sleep 0.0$i
done &
wait
