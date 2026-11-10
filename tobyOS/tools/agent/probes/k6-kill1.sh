# K6: kill(-1) must not signal the caller (Linux kill(2)). Detached: the
# probe's own session is killed by it; the answer is left in a file.
rm -f /tmp/k6.out
bash -c 'sleep 30 & kill -TERM -1; echo "caller survived: $?" > /tmp/k6.out' &
