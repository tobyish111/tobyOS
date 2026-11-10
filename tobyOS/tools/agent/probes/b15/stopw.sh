# stopw.sh: a process BLOCKED in the kernel -- waiting for its child,
# reading a pipe -- is stopped by a stop signal whose action is the
# default, and once continued carries on as if nothing happened (Linux:
# the call returns ERESTARTSYS, the stop is taken on the way out, and the
# call restarts). Item 5: ^Z of `{ sleep 30; } | cat` hung both shells,
# their subshells blocked in wait4 never stopping. STOP and TSTP both: on
# Linux a TSTP to an orphaned process group is discarded, which a script
# run without a terminal is in, so WSL shows S for the TSTP lines.
exec 2>&1
st() { sed -n 's/^State:[[:space:]]*\(.\).*/\1/p' /proc/$1/status 2>/dev/null; }
( sleep 3; echo "child done" ) & sp=$!
sleep 0.5; kill -STOP $sp; sleep 0.5; echo "waiter after STOP: $(st $sp)"
kill -CONT $sp; sleep 0.3; echo "waiter after CONT: $(st $sp)"
wait $sp; echo "wait=$?"
{ sleep 2; echo hi; } | { read x; echo "read [$x] $?"; } & rp=$!
sleep 0.5; kill -STOP $rp; sleep 0.5; echo "reader after STOP: $(st $rp)"
kill -CONT $rp; wait $rp; echo "wait=$?"
{ sleep 2; echo hi; } | cat & cp=$!
sleep 0.5; kill -STOP $cp; sleep 0.5; echo "cat after STOP: $(st $cp)"
kill -CONT $cp; wait $cp; echo "wait=$?"
sleep 2 & zp=$!
sleep 0.3; kill -STOP $zp; sleep 0.3; echo "sleep after STOP: $(st $zp)"
kill -CONT $zp; wait $zp; echo "wait=$?"
echo end
