# r15: `wait` with no operand waits for every process of a background
# pipeline, with and without job control
exec 2>&1
cd /tmp
for m in +m -m; do
  set $m
  bad=0
  for i in 1 2 3 4 5 6 7 8 9 10; do
    rm -f r15o
    echo hi-pipe | cat > r15o &
    wait
    read -r l < r15o || bad=$((bad+1))
    [ "$l" = hi-pipe ] || bad=$((bad+1))
    l=
  done
  echo "$m bad=$bad"
  bad=0
  for i in 1 2 3 4 5; do
    rm -f r15o
    sleep 0 | { sleep 1; echo late > r15o; } &
    wait
    [ -e r15o ] || bad=$((bad+1))
  done
  echo "$m late-bad=$bad"
done
echo end
