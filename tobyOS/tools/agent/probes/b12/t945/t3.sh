sleep 0.1 &
sleep 0.2 &
kill -15 %-
echo kill=$?
e=$((SECONDS+2)); while ((SECONDS<e)); do :; done
jobs
wait %-
echo wait=$?
