exec 2>&1
times | sed 's/[0-9]/N/g'
times foo | sed 's/[0-9]/N/g'
times -x; echo "rc=$?"
i=0; while ((i<60000)); do ((i++)); done
times | sed -n 1p | awk '{print ($1 != "0m0.000s") ? "self-user-nonzero" : "self-user-zero"}'
/bin/sh -c 'i=0; while [ $i -lt 20000 ]; do i=$((i+1)); done'
times | sed -n 2p | awk '{print ($1 != "0m0.000s") ? "kids-user-nonzero" : "kids-user-zero"}'
TIMEFORMAT='%3lR|%3lU|%3lS|%P|%2R|%0U|%E'
{ time /bin/sh -c 'i=0; while [ $i -lt 20000 ]; do i=$((i+1)); done'; } 2>&1 | sed 's/[0-9]/N/g'
{ time /bin/sh -c 'i=0; while [ $i -lt 20000 ]; do i=$((i+1)); done'; } 2>&1 | awk -F'|' '{print ($2 != "0m0.000s") ? "time-user-nonzero" : "time-user-zero"}'
set -- $(cut -d' ' -f14-17 /proc/$$/stat); echo "stat fields=$#"
echo end
