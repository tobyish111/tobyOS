# fxiso.sh FIRST LAST RUNS -- the image's OWN fixtures (/etc/shparity) from
# FIRST to LAST, each RUNS times under the guest's bash and /bin/tsh, as the
# gate compares them (stdout and exit status); one line a fixture: SAME,
# DIFF, or VARIES when a shell does not agree with itself.
first=${1:-281}; last=${2:-293}; runs=${3:-3}
cd /tmp || exit 1
for f in /etc/shparity/*.sh; do
  n=${f##*/}; n=${n%%-*}
  case $n in *[!0-9]*) continue ;; esac
  [ "$n" -ge "$first" ] && [ "$n" -le "$last" ] || continue
  for sh in bash tsh; do
    i=0; : > /tmp/fx.$sh.all
    while [ $i -lt "$runs" ]; do i=$((i+1))
      SH=$sh $sh "$f" > /tmp/fx.$sh.$i 2>/dev/null < /dev/null; echo "exit=$?" >> /tmp/fx.$sh.$i
      md5sum < /tmp/fx.$sh.$i >> /tmp/fx.$sh.all
    done
  done
  b=$(sort -u /tmp/fx.bash.all | wc -l); t=$(sort -u /tmp/fx.tsh.all | wc -l)
  if [ "$b" -ne 1 ] || [ "$t" -ne 1 ]; then echo "$n VARIES bash=$b tsh=$t"
  elif cmp -s /tmp/fx.bash.1 /tmp/fx.tsh.1; then echo "$n SAME"
  else echo "$n DIFF"; fi
done
rm -f /tmp/fx.*
