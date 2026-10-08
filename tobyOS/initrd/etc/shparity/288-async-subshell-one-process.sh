# `( LIST ) &` IS ONE PROCESS, the ( ) itself (bash's execute_in_subshell):
# $! is its pid, BASH_SUBSHELL inside is 1, and killing $! ends the list.
# tsh forked a second time to run it, so $! named a wrapper and the list ran
# on after `kill $!` (logs/b15 p6 j1-j5).
exec 2>&1
( echo "pid=$BASHPID" > /tmp/sp288 ) & p=$!
wait
[ "$(cat /tmp/sp288)" = "pid=$p" ] && echo "j1 same" || echo "j1 differs"
rm -f /tmp/sp288
( echo "j2 $BASH_SUBSHELL" ) & wait
{ echo "j3 $BASH_SUBSHELL"; } & wait
( ( echo "j4 $BASH_SUBSHELL" ) ) & wait
( sleep 2; echo j5-ran-on ) & p=$!
sleep 0.3; kill $p; wait $p; echo "j5 $?"; sleep 2
( trap 'echo j6-exit' EXIT; echo j6-body ) & wait
echo end
