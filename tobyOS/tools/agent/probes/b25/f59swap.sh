#!/bin/bash
# f59.sh -- finding 59: swap /bin/tsh over SSH, then small uploads; md5 each
cd /c/CustomOS/tobyOS
export ED_HOST=127.0.0.1 ED_PORT=2222
want=$(md5sum < programs/tsh/tsh.elf | cut -c1-32)
for try in 1 2 3; do
    bash logs/edremote.sh 'cat > /tmp/tsh.new' < programs/tsh/tsh.elf >/dev/null 2>&1
    got=$(bash logs/edremote.sh 'md5sum < /tmp/tsh.new' 2>/dev/null | tr -d '\r' | cut -c1-32)
    echo "tsh upload try $try: $got (want $want)"
    [ "$got" = "$want" ] && break
done
[ "$got" = "$want" ] || { echo "no intact upload; not swapping"; exit 1; }
ED_USER=root bash logs/edremote.sh 'cp /tmp/tsh.new /bin/tsh && chmod 755 /bin/tsh && echo swapped' 2>&1 | tr -d '\r'
head -c 3000 /dev/urandom > /c/t/b25/small.bin
sw=$(md5sum < /c/t/b25/small.bin | cut -c1-32)
for i in $(seq 1 12); do
    t0=$(date +%s.%N)
    bash logs/edremote.sh "cat > /tmp/up$i" < /c/t/b25/small.bin >/dev/null 2>&1
    rc=$?
    g=$(bash logs/edremote.sh "md5sum < /tmp/up$i; wc -c < /tmp/up$i" 2>/dev/null | tr -d '\r' | tr '\n' ' ')
    t1=$(date +%s.%N)
    echo "upload $i rc=$rc: $g $( [ "${g:0:32}" = "$sw" ] && echo OK || echo BAD) ($(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.2f", b - a }')s)"
done
