# r17: a background function that itself runs `cmd & wait`, under set -m
exec 2>&1
cd /tmp
set -m
k() { sleep 1 & wait; echo k-done; }
k > r17o &
wait
echo "after-wait $(cat r17o)"
echo end
