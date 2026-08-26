{ sleep 0.08; exit 8; } &
jid1=$!
{ sleep 0.09; exit 9; } &
jid2=$!
{ sleep 0.07; exit 7; } &
jid3=$!
wait $jid1 $jid2 $jid3  # NOTE: not using %1 %2 %3 syntax on purpose
echo "status=$?"  # third job I think
