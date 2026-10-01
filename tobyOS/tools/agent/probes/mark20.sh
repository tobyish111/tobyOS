# bash's mark on a finished job, 30 runs (S9/M1): '+' or ' ' ?
printf 'false & sleep 0.2; jobs\n' > /tmp/mark1.sh
for i in $(seq 30); do bash /tmp/mark1.sh | cut -c1-4; done | sort | uniq -c
rm -f /tmp/mark1.sh
