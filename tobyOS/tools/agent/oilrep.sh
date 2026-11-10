#!/bin/bash
# oilrep.sh N CASE... -- each Oils case N times under bash and under tsh on
# the guest (fresh directory each run, stdin /dev/null, SH set), one line per
# run: CASE SHELL stdout-joined-with-|  (stderr dropped, as the gate compares
# stdout and exit).
cd /c/CustomOS/tobyOS || exit 1
export ED_HOST=${ED_HOST:-127.0.0.1} ED_PORT=${ED_PORT:-2222} ED_USER=${ED_USER:-root} ED_TIMEOUT=${ED_TIMEOUT:-300}
n=$1; shift
for c in "$@"; do
  for sh in bash tsh; do
    bash logs/edremote.sh "mkdir -p /data/orep; i=0; while [ \$i -lt $n ]; do i=\$((i+1)); rm -rf /data/orep/w; mkdir -p /data/orep/w; cd /data/orep/w; out=\$(SH=$sh $sh /etc/oilspec/$c.sh 2>/dev/null < /dev/null; echo \"exit=\$?\"); echo \"$c $sh \$(echo \"\$out\" | tr '\n' '|')\"; cd /; done" 2>/dev/null | tr -d '\r' | grep "^$c "
  done
done
