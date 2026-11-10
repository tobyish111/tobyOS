# A freed pid used to come straight back: the next process took the lowest
# free slot. bash keeps finished jobs' pids, so a pid of one background
# pipeline reappearing in the next made it credit exits to the wrong job.
echo hi | { exit 99; } &
wait $!; echo "first job: $?"
pids=''
for i in 3 2 1; do
  sleep 0.0$i | echo i=$i | ( exit $i ) &
  pids="$pids $!"
done
for pid in $pids; do wait $pid; echo "status=$?"; done
a=$(/bin/sh -c 'echo $$'); b=$(/bin/sh -c 'echo $$')
[ "$a" != "$b" ] && echo "two processes in a row get two pids"
echo done
