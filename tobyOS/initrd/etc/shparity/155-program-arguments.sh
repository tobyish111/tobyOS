a100=$(printf '%s ' $(seq 100)); a5000=$(printf '%s ' $(seq 5000))
echo "1 $(/bin/echo $a100 | wc -w)"
echo "2 $(/bin/echo $a5000 | wc -w)"
long=$(printf 'x%.0s' $(seq 10000)); echo "3 $(/bin/echo "$long" | wc -c)"
big=$(printf 'y%.0s' $(seq 200000)); /bin/echo "$big" > /dev/null 2>&1; echo "4 rc=$?"
i=0; while [ $i -lt 150 ]; do export "EV_$i=v$i"; i=$((i+1)); done
echo "5 $(env | grep -c '^EV_')"
sh -c 'echo "6 $#"' _ $a100
echo "7 $(printf '%s\n' $a5000 | xargs /bin/echo | wc -w)"
echo "x1 $(seq 5000 | xargs /bin/echo | wc -w)"
echo "x2 $(seq 5000 | xargs -n 1000 /bin/echo | wc -l)"
echo "x3 $(seq 100000 | xargs /bin/echo | wc -w)"
big=$(printf 'y%.0s' $(seq 200000)); /bin/echo "$big" > /dev/null 2>&1; echo "e rc=$?"
f=/tmp/many.$$; i=3; while [ $i -le 30 ]; do eval "exec $i>$f.$i"; i=$((i+1)); done
echo "fds $(ls /proc/self/fd | wc -l)"
i=3; while [ $i -le 30 ]; do eval "exec $i>&-"; rm -f $f.$i; i=$((i+1)); done
env -i A=1 B=2 /bin/sh -c 'echo "ei $A$B"'
echo done
