/bin/sh -c 'i=0; while [ $i -lt 50000 ]; do i=$((i+1)); done; cut -d" " -f14-17 /proc/$$/stat'
times
cut -d' ' -f14-17 /proc/$$/stat
bash -c 'i=0; while [ $i -lt 50000 ]; do i=$((i+1)); done; times'
times
