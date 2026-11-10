# r245.sh: shparity fixture 245a ten times under each shell named in $SHELLS;
# the named coprocess's fd line and anything else that differs from run 1.
for sh in ${SHELLS:-bash /bin/tsh}; do
  first=""; same=0; diff=0; seen=""
  for i in 1 2 3 4 5 6 7 8 9 10; do
    out=$($sh /etc/shparity/245a-coproc.sh 2>&1)
    l6=$(printf '%s\n' "$out" | sed -n 6p)
    seen="$seen [$l6]"
    if [ -z "$first" ]; then first=$out; same=1
    elif [ "$out" = "$first" ]; then same=$((same+1)); else diff=$((diff+1)); fi
  done
  echo "$sh: same-as-run-1 $same, other $diff;$seen"
done
