# o0488.sh: Oils 0488 twenty times under each shell, each run in a fresh
# directory as the harness runs it; counts of each outcome.
for sh in bash /bin/tsh; do
  ok=0; bad=0; other=""
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    d=/tmp/o488.$$.$i; mkdir -p $d; cd $d
    out=$($sh /etc/oilspec/0488.sh 2>/dev/null | tr '\n' ' ')
    cd /; rm -rf $d
    if [ "$out" = "status=129 Hangup " ]; then ok=$((ok+1)); else bad=$((bad+1)); other="$other[$out]"; fi
  done
  echo "$sh: as bash's oracle $ok, other $bad $other"
done
