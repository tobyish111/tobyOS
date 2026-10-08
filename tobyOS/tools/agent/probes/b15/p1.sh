# p1.sh SHELL... -- when each shell notices a job it has just killed, and
# Oils 0944 0945 0488; each case five times, outcomes counted
cases() {
  case $1 in
  c1) printf '%s\n' 'sleep 5 &' 'kill -HUP $!' 'jobs' 'echo end' ;;
  c2) printf '%s\n' 'sleep 5 &' 'kill -HUP $!' ':' 'jobs' 'echo end' ;;
  c3) printf '%s\n' 'sleep 5 &' 'kill -HUP $!; jobs' 'echo end' ;;
  c4) printf '%s\n' 'sleep 5 &' 'kill -HUP $!' 'wait $! 2>/tmp/p1err; echo "w=$? err=[$(tr -d "\n" < /tmp/p1err)]"' ;;
  c5) printf '%s\n' 'sleep 0.5 &' 'kill -15 %%' 'echo kill=$?' 'wait %%' 'echo wait=$?' ;;
  c6) printf '%s\n' 'sleep 5 &' 'kill -USR1 $!' ':' 'jobs' 'echo end' ;;
  c7) printf '%s\n' 'sleep 5 &' 'kill -9 $!' 'jobs' 'echo end' ;;
  o0944) cat /etc/oilspec/0944.sh ;;
  o0945) cat /etc/oilspec/0945.sh ;;
  o0488) cat /etc/oilspec/0488.sh ;;
  esac
}
cd /tmp
for c in c1 c2 c3 c4 c5 c6 c7 o0944 o0945 o0488; do
  cases $c > /tmp/p1case.sh
  for sh in "$@"; do
    i=0; while [ $i -lt 5 ]; do i=$((i+1))
      rm -f err.txt
      r=$(SH=$sh $sh /tmp/p1case.sh 2>&1 < /dev/null | sed -E 's/[0-9]+ /N /g; s/line [0-9]+/line L/; s/  +/ /g' | tr '\n' '|')
      echo "$c $sh $r"
    done
  done
done | sort | uniq -c
