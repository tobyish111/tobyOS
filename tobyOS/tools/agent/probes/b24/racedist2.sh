#!/bin/bash
# racedist2.sh N ID... -- racedist.sh with the shell LEADING ITS OWN PROCESS
# GROUP, as programs/oilspec runs it (setpgid(0, 0) in its fork; here `set
# -m` puts each foreground job in a group of its own). Each shell's distinct
# answers -- stdout and exit status -- with their counts. Under bash, as root.
N=$1; shift
set -m
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
