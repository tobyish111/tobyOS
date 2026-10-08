# SIGINT WHILE A SCRIPT WAITS FOR A FOREGROUND COMMAND (bash's
# wait_sigint_handler, "wait and cooperative exit"): the shell notes it and
# waits on; if the command died of it the shell acts on it too (EXIT trap,
# death by SIGINT), and if the command caught it the script goes on. tsh
# died the moment it came (logs/b15 p6 i1-i4). set -m gives each child a
# group of its own, so INT is not ignored in it.
exec 2>&1
set -m
"$BASH" -c 'trap "echo i1-exit" EXIT; sleep 3; echo i1-after' & p=$!
set +m
sleep 1; kill -INT $p; wait $p; echo "i1 $?"
set -m
"$BASH" -c 'sleep 3; echo i2-after' & p=$!
set +m
sleep 1; kill -INT $p; wait $p; echo "i2 $?"
set -m
"$BASH" -c 'trap "echo i3-exit" EXIT; sleep 3; echo i3-after' & p=$!
set +m
sleep 1; kill -INT -$p; wait $p; echo "i3 $?"
set -m
"$BASH" -c 'trap "echo i4-exit" EXIT; "$BASH" -c "trap \"exit 0\" INT; sleep 3 & wait"; echo "i4-after $?"' & p=$!
set +m
sleep 1; kill -INT -$p; wait $p; echo "i4 $?"
set -m
"$BASH" -c 'trap "echo i5-trap" INT; sleep 3; echo i5-after' & p=$!
set +m
sleep 1; kill -INT $p; wait $p; echo "i5 $?"
echo end
