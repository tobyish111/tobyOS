# Finding 21 in bash's own shape: two children that exit together, so the
# second SIGCHLD is pending when bash's handler returns -- what killed bash
# (exit 139) in shparity 146. Runs bash on a loop of such pairs RUNS times
# and counts how each run ended. Usage: RUNS=20 PAIRS=200 sh this.
RUNS=${RUNS:-20}; PAIRS=${PAIRS:-200}
cat > /tmp/b21.sh <<EOF
i=0
while [ \$i -lt $PAIRS ]; do
  ( : ) & ( : ) &
  x=\$(printf '%s' abcdefghijklmnopqrstuvwxyz0123456789)
  wait
  i=\$((i+1))
done
echo ok
EOF
ok=0 segv=0 other=0 r=0
while [ $r -lt $RUNS ]; do
  bash /tmp/b21.sh > /tmp/b21.out 2>&1; st=$?
  if [ $st -eq 0 ] && grep -q '^ok$' /tmp/b21.out; then ok=$((ok+1))
  elif [ $st -eq 139 ]; then segv=$((segv+1))
  else other=$((other+1)); fi
  r=$((r+1))
done
echo "bash pairs: runs=$RUNS ok=$ok segv=$segv other=$other"
rm -f /tmp/b21.sh /tmp/b21.out
