# A shell that started a background job leaves WITHOUT waiting for it, as
# bash does: the job is orphaned and runs on. /bin/tsh used to wait for every
# job (3004 ms for `tsh -c 'sleep 3 &'`, bash 3 ms) -- a script that started
# a daemon hung until the daemon exited. The orphan must still RUN: both
# words of `echo word_a & echo word_b` arrive (their order is a race in bash
# too, so they are sorted).
self=/bin/tsh; [ -n "$BASH_VERSION" ] && self=bash
s=$(date +%s); $self -c 'sleep 3 &'; e=$(date +%s)
if [ $((e - s)) -lt 2 ]; then echo "-c: returned at once"; else echo "-c: waited for its job"; fi
printf 'sleep 3 &\necho started\n' > /tmp/fx169.sh
s=$(date +%s); $self /tmp/fx169.sh; e=$(date +%s)
if [ $((e - s)) -lt 2 ]; then echo "script: returned at once"; else echo "script: waited for its job"; fi
$self -c 'echo word_a & echo word_b' | sort
$self -c 'sleep 1 & echo "bg=$!"' | sed 's/[0-9][0-9]*/N/'
rm -f /tmp/fx169.sh
echo done
