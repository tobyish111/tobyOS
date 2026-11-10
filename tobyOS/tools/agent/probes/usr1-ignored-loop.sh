# shparity 156's last line, looped: a background job started while USR1 is
# ignored must keep ignoring it across its exec -- `wait` then sees the TERM
# (143), never USR1 (138). Prints how often each status came back.
N=${N:-40}
trap "" USR1
t143=0 t138=0 other=0 i=0
while [ $i -lt $N ]; do
  sleep 3 & p=$!
  kill -USR1 $p
  sleep 0.2
  kill -TERM $p
  wait $p; st=$?
  case $st in 143) t143=$((t143+1));; 138) t138=$((t138+1));; *) other=$((other+1));; esac
  i=$((i+1))
done
trap - USR1
echo "143(TERM)=$t143 138(USR1)=$t138 other=$other of $N"
