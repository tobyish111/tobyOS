exec 2>&1
busy() { e=$((SECONDS+2)); while ((SECONDS<e)); do :; done; }
sleep 1 & kill -9 $!; busy; trap 'true' DEBUG; echo "a debug"; trap - DEBUG
echo "a next"
sleep 1 & kill -9 $!; busy; trap 'true' ERR; false; echo "b err"; trap - ERR
echo "b next"
r() { :; }
sleep 1 & kill -9 $!; busy; trap 'true' RETURN; r; echo "c return"; trap - RETURN
echo "c next"
sleep 1 & kill -9 $!; busy; trap 'echo usr1' USR1; kill -USR1 $$; echo "d usr1"
echo "d next"
trap 'echo bye' EXIT
sleep 1 & kill -9 $!; busy; exit 0
