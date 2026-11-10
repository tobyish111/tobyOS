# sigjobs.sh -- the job-status word bash's `jobs` prints for a job killed by
# INT, PIPE, TERM, KILL (the words its kill+wait reports never show).
for s in 2 13 15 9 1; do sleep 30 & kill -$s $!; sleep 0.3; jobs; done
echo done
