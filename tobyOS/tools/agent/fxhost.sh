#!/bin/bash
# fxhost.sh OUT -- every fixture in FXDIR (default C:\t\b12\fx), already
# uploaded to /data/fxw3 on the guest, one SSH call
# each: bash twice and tsh once in fresh directories (stdin /dev/null, 60 s
# limit), compared on the guest. One line per fixture in OUT:
#   NAME SAME|DIFF DET|NONDET LINES
cd /c/CustomOS/tobyOS || exit 1
export ED_HOST=127.0.0.1 ED_PORT=2222 ED_USER=root ED_TIMEOUT=240
OUT=$1
: > "$OUT"
for f in ${FXDIR:-/c/t/b12/fx}/*.sh; do
  n=$(basename "$f" .sh)
  line=$(bash logs/edremote.sh "cd /data/fxw3 && mkdir -p out wd && for r in b1 b2 t1; do case \$r in b*) sh=bash;; *) sh=tsh;; esac; rm -rf wd/$n.\$r; mkdir -p wd/$n.\$r; ( cd wd/$n.\$r && SH=\$sh timeout 60 \$sh /data/fxw3/$n.sh > /data/fxw3/out/$n.\$r 2>&1 < /dev/null; echo exit=\$? >> /data/fxw3/out/$n.\$r ); done; s=SAME; cmp -s out/$n.b1 out/$n.t1 || s=DIFF; d=DET; cmp -s out/$n.b1 out/$n.b2 || d=NONDET; echo \"RES $n \$s \$d \$(wc -l < out/$n.b1)\"" 2>&1 | tr -d '\r' | grep "^RES " | sed 's/^RES //')
  [ -n "$line" ] || line="$n NORESULT"
  echo "$line" >> "$OUT"
done
awk '{print $2, $3}' "$OUT" | sort | uniq -c
grep -v "SAME DET" "$OUT"
