sleep 1 &
kill -HUP $!
jobs
wait $! 2>err.txt
echo status=$?
cat err.txt
