# r16: a background pipeline under set -x (and set -m)
exec 2>&1
cd /tmp
rm -f r16a r16b r16c
set -x
echo hi-pipe | cat > r16a &
wait
set -m
echo hi-pipe2 | cat > r16b &
wait
f() { echo in-f; }
f | cat > r16c &
wait
set +x +m
echo "a=[$(cat r16a)] b=[$(cat r16b)] c=[$(cat r16c)]"
echo end
