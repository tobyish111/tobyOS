# most likely 0 seconds, but in CI I've seen 1 second
echo $SECONDS | awk '/[0-9]+/ { print "ok" }'
