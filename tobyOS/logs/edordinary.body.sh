# The body of the ordinary-work loop, run ON THE BOX by logs/edordinary.sh,
# which prefixes N=<iterations>. Every step is stamped to /data BEFORE it
# runs, so a wedge names its own iteration and command.
: > /data/ordinary.txt
i=0
while [ "$i" -lt "$N" ]; do
    i=$((i+1))
    f=/tmp/ord_$i
    echo "$i create" >> /data/ordinary.txt;  printf 'hello %s\n' "$i" > "$f"
    echo "$i chmod"  >> /data/ordinary.txt;  chmod 755 "$f"
    echo "$i stat"   >> /data/ordinary.txt;  stat -c '%a %s' "$f" > /dev/null
    echo "$i pipe"   >> /data/ordinary.txt;  cat "$f" | tr a-z A-Z | wc -c > /dev/null
    echo "$i spawn"  >> /data/ordinary.txt;  /bin/true; date +%s > /dev/null
    echo "$i ps"     >> /data/ordinary.txt;  ps -o pid= -C tsh > /dev/null
    echo "$i ls"     >> /data/ordinary.txt;  ls -l /tmp > /dev/null
    echo "$i rm"     >> /data/ordinary.txt;  rm -f "$f"
    echo "$i sleep"  >> /data/ordinary.txt;  sleep 1
    [ $((i % 100)) -eq 0 ] && echo "ORDINARY-PROGRESS $i"
done
echo "done $i" >> /data/ordinary.txt
echo "ORDINARY-DONE $i"
