#!/bin/bash
# oilcases.sh OUT ID... -- run Oils corpus cases (/etc/oilspec/ID.sh on the
# guest) under bash and /bin/tsh the way programs/oilspec does: the same
# environment (env -i PATH=/etc/oilspec/bin:/bin HOME=/ TERM=dumb LC_ALL=C
# TMP=TMPDIR=<scratch>), a fresh working directory, stdin /dev/null. Writes
# OUT with, per case, each shell's stdout, stderr and exit status, and a
# SAME/DIFF line on stdout+exit (the gate's rule). The driver runs under the
# guest's BASH (so a tsh bug cannot garble the report -- one did: `>>` from
# a group), writes a file on the guest, and it comes back gzipped and
# base64'd 45 lines a call (an edremote reply is cut short after a few KB),
# checked against the guest's md5sum.
cd "$(dirname "$0")/../.." || exit 1
OUT=$1; shift
cat > /tmp/oilcases-driver.$$ <<'EOF'
run1() { sh=$1; id=$2; rm -rf /tmp/oc; mkdir -p /tmp/oc/w /tmp/oc/t; cd /tmp/oc/w
  if [ "$sh" = bash ]; then set -- /bin/bash --norc --noprofile /etc/oilspec/$id.sh; else set -- /bin/tsh /etc/oilspec/$id.sh; fi
  env -i PATH=/etc/oilspec/bin:/bin HOME=/ TERM=dumb LC_ALL=C TMP=/tmp/oc/t TMPDIR=/tmp/oc/t TSH_FDTRACE=1 "$@" > /tmp/oc.out 2> /tmp/oc.err < /dev/null
  echo $? > /tmp/oc.rc; cd /; }
: > /tmp/ocres
for id in "$@"; do
  run1 bash $id; cp /tmp/oc.out /tmp/oc.bo; cp /tmp/oc.err /tmp/oc.be; brc=$(cat /tmp/oc.rc)
  run1 tsh $id; trc=$(cat /tmp/oc.rc)
  if cmp -s /tmp/oc.bo /tmp/oc.out && [ "$brc" = "$trc" ]; then v=SAME; else v=DIFF; fi
  { echo "######## $id $v (exit bash=$brc tsh=$trc)"; echo "--- bash stdout"; cat /tmp/oc.bo; echo "--- tsh stdout"; cat /tmp/oc.out
    echo "--- bash stderr"; cat /tmp/oc.be; echo "--- tsh stderr"; cat /tmp/oc.err; } >> /tmp/ocres
done
gzip -c /tmp/ocres | base64 > /tmp/ocres.b64
echo "$(wc -l < /tmp/ocres.b64) $(md5sum < /tmp/ocres | cut -c1-32)"
EOF
bash logs/edremote.sh "cat > /tmp/ocdriver.sh" < /tmp/oilcases-driver.$$ >/dev/null 2>&1
rm -f /tmp/oilcases-driver.$$
info=$(ED_TIMEOUT=${ED_TIMEOUT:-900} bash logs/edremote.sh "bash /tmp/ocdriver.sh $*" 2>/dev/null | tr -d '\r' | tail -1)
lines=${info%% *}; sum=${info##* }
B64=$(mktemp)
: > "$B64"
start=1
while [ "$start" -le "${lines:-0}" ]; do
  bash logs/edremote.sh "sed -n '${start},$((start+44))p' /tmp/ocres.b64" 2>/dev/null | tr -d '\r' >> "$B64"
  start=$((start+45))
done
base64 -d < "$B64" | gzip -dc > "$OUT"
rm -f "$B64"
got=$(md5sum < "$OUT" | cut -c1-32)
if [ "$got" != "$sum" ]; then echo "oilcases: TRANSFER MISMATCH (guest $sum, host $got)" >&2; exit 2; fi
grep -a "^######## " "$OUT"
