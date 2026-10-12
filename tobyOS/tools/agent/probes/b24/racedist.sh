#!/bin/bash
# racedist.sh N ID... -- run each Oils case N times under bash and N times
# under /bin/tsh the way programs/oilspec does (env -i PATH=/etc/oilspec/bin:
# /bin HOME=/ TERM=dumb LC_ALL=C TMP=TMPDIR=scratch, a fresh working
# directory, stdin /dev/null), alternating the shells, and print each
# shell's distinct answers -- stdout and exit status, the gate's contract --
# with their counts. Run on the guest under bash. RACE_LOAD=K adds K busy
# loops for the duration.
N=$1; shift
for k in $(seq 1 "${RACE_LOAD:-0}"); do ( while :; do :; done ) & done
run1() { sh=$1; id=$2; rm -rf /tmp/rd; mkdir -p /tmp/rd/w /tmp/rd/t; cd /tmp/rd/w
  if [ "$sh" = bash ]; then set -- /bin/bash --norc --noprofile /etc/oilspec/$id.sh; else set -- /bin/tsh /etc/oilspec/$id.sh; fi
  env -i PATH=/etc/oilspec/bin:/bin HOME=/ TERM=dumb LC_ALL=C TMP=/tmp/rd/t TMPDIR=/tmp/rd/t "$@" > /tmp/rd.out 2> /dev/null < /dev/null
  echo "rc=$?" >> /tmp/rd.out; cd /; }
for id in "$@"; do
  rm -rf /tmp/rdans; mkdir -p /tmp/rdans
  for i in $(seq 1 "$N"); do
    for sh in bash tsh; do
      run1 $sh $id
      key=$(md5sum < /tmp/rd.out | cut -c1-8)
      cp /tmp/rd.out /tmp/rdans/$key
      echo "$sh $key" >> /tmp/rdans/tally
    done
  done
  echo "######## $id"
  for key in $(cut -d' ' -f2 /tmp/rdans/tally | sort -u); do
    b=$(grep -c "^bash $key" /tmp/rdans/tally); t=$(grep -c "^tsh $key" /tmp/rdans/tally)
    echo "-- $key bash=$b tsh=$t: $(tr '\n' '|' < /tmp/rdans/$key)"
  done
done
for j in $(jobs -p); do kill $j; done
