# p4: the EXIT trap and fatal signals -- when, and for which (bash's
# initialize_terminating_signals / termsig_handler). Each child is started
# with `&`, so it starts with INT and QUIT ignored.
exec 2>&1
echo "== h1: EXIT trap, TERM while a foreground child runs"
"$BASH" -c 'trap "echo h1-exit" EXIT; sleep 4; echo h1-after' & p=$!
sleep 1; s=$SECONDS; kill -TERM $p; wait $p; echo "h1 st=$? waited=$((SECONDS-s))"
echo "== h2: EXIT trap, TERM during a builtin loop"
"$BASH" -c 'trap "echo h2-exit" EXIT; while :; do :; done' & p=$!
sleep 1; kill -TERM $p; wait $p; echo "h2 st=$?"
echo "== h3: EXIT trap, TERM during read"
sleep 4 | "$BASH" -c 'trap "echo h3-exit" EXIT; read x; echo h3-after' & p=$!
sleep 1; s=$SECONDS; kill -TERM $p; wait $p; echo "h3 st=$? waited=$((SECONDS-s))"
wait
echo "== h4: trapped TERM while a foreground child runs"
"$BASH" -c 'trap "echo h4-trap" TERM; sleep 3; echo h4-after' & p=$!
sleep 1; s=$SECONDS; kill -TERM $p; wait $p; echo "h4 st=$? waited=$((SECONDS-s))"
echo "== h5: trapped TERM during read"
sleep 4 | "$BASH" -c 'trap "echo h5-trap" TERM; read x; echo "h5-after rc=$?"' & p=$!
sleep 1; s=$SECONDS; kill -TERM $p; wait $p; echo "h5 st=$? waited=$((SECONDS-s))"
wait
echo "== h6: a subshell with its own EXIT trap"
"$BASH" -c 'trap "echo h6-outer" EXIT; ( trap "echo h6-inner" EXIT; sleep 3; echo h6-sub-after ) & q=$!; sleep 1; kill -TERM $q; wait $q; echo "h6 sub st=$?"'
"$BASH" -c 'trap "echo h6b-outer" EXIT; ( sleep 3; echo h6b-sub-after ) & q=$!; sleep 1; kill -TERM $q; wait $q; echo "h6b sub st=$?"'
echo "== h7: an async child sets an EXIT trap, then traps INT"
{ trap 'echo h7-exit' EXIT; trap 'echo h7-got' INT; kill -INT $BASHPID; echo h7-after; } & wait
echo "h7 st=$?"
echo "== h8: EXIT trap, then a USR1 trap and its reset"
"$BASH" -c 'trap "echo h8-exit" EXIT; trap "echo u" USR1; trap - USR1; kill -USR1 $$; echo h8-after'; echo "h8 st=$?"
"$BASH" -c 'trap "echo u" USR1; trap "echo h8b-exit" EXIT; trap - USR1; kill -USR1 $$; echo h8b-after'; echo "h8b st=$?"
echo "== h9: EXIT trap and SIGPIPE"
"$BASH" -c 'trap "echo h9-exit >&2" EXIT; while :; do echo y; done' | head -1; echo "h9 st=${PIPESTATUS[0]}"
echo "== h10: EXIT trap and other signals"
for s in HUP USR2 ALRM VTALRM XCPU XFSZ ABRT PROF WINCH CHLD URG; do
  "$BASH" -c 'trap "echo exit-'$s'" EXIT; kill -'$s' $$; echo after-'$s; echo "h10 $s st=$?"
done
echo "== h11: no EXIT trap, TERM while a foreground child runs"
"$BASH" -c 'sleep 3; echo h11-after' & p=$!
sleep 1; s=$SECONDS; kill -TERM $p; wait $p; echo "h11 st=$? waited=$((SECONDS-s))"
echo "== h12: trap - EXIT, then TERM"
"$BASH" -c 'trap "echo h12-exit" EXIT; trap - EXIT; kill -TERM $$; echo h12-after'; echo "h12 st=$?"
echo "== h13: EXIT trap, INT not ignored at entry (foreground child shell)"
trap - INT
"$BASH" -c 'trap "echo h13-exit" EXIT; kill -INT $$; echo h13-after'; echo "h13 st=$?"
echo "== h14: the EXIT trap and exit in a TERM trap"
"$BASH" -c 'trap "echo h14-exit" EXIT; trap "echo h14-term; exit 7" TERM; kill -TERM $$; echo h14-after'; echo "h14 st=$?"
echo "== h15: the EXIT trap runs once when it is itself killed"
"$BASH" -c 'trap "echo h15-exit; kill -TERM \$\$; echo h15-exit-after" EXIT; kill -TERM $$; echo h15-after'; echo "h15 st=$?"
echo end
