exec 2>&1
sleep 0.3 & wait; echo "a:"; jobs
( sleep 0.3 ) & wait; echo "b:"; jobs
sleep 0.6 & sleep 0.1 & wait; echo "c:"; jobs; echo "c2:"; jobs
sleep 0.1 & sleep 0.6; wait; echo "d:"; jobs
sleep 0.2 & wait $!; echo "e:"; jobs
sleep 1 & { sleep 1; } >/dev/null & ( sleep 0.2; echo y ) >/dev/null & wait; echo "f:"; jobs
