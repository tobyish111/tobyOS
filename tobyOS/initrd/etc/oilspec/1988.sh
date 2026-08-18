cat <(seq 3; sleep 0.1) & wait

echo sync

# This one escapes, and the shell should still exit
cat <(sleep 0.1) &

echo fork
