exec 2>&1
s=$SECONDS; sleep 4 | { read -t 2 x; echo "a pipe rc=$? w=$((SECONDS-s))"; }
s=$SECONDS; read -t 2 x < <(sleep 4); echo "b procsub rc=$? w=$((SECONDS-s))"
rm -f /tmp/rtff; mkfifo /tmp/rtff; (sleep 4 > /tmp/rtff) &
s=$SECONDS; read -t 2 x < /tmp/rtff; echo "c fifo rc=$? w=$((SECONDS-s))"; wait
s=$SECONDS; read -t 2 x < <(sleep 4; echo z); echo "d procsub-data rc=$? w=$((SECONDS-s))"
s=$SECONDS; { read -t 2 x; echo "e brace-pipe rc=$? w=$((SECONDS-s))"; } < <(sleep 4)
echo end
