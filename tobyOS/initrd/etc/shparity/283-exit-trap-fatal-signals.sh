# THE EXIT TRAP RUNS WHEN A FATAL SIGNAL ENDS THE SHELL (bash's
# initialize_terminating_signals and termsig_handler): once a
# non-interactive shell has an EXIT trap, a terminating signal that is
# neither trapped nor ignored runs it, and the shell then dies OF the
# signal -- mid-wait for a foreground child, mid-read, in a loop, a
# function, a subshell or a $( ) with a trap of its own; $? is the last
# command's; a second one during the trap ends it at once. tsh just died
# (logs/b15 p4, p6 x, p7 e). Job reports go to /dev/null: they name pids.
exec 2>&1
"$BASH" -c 'trap "echo g1-exit" EXIT; kill -TERM $$; echo g1-after'; echo "g1 $?"
"$BASH" -c 'trap "echo g2-exit" EXIT; kill -INT $$; echo g2-after'; echo "g2 $?"
trap '' INT
"$BASH" -c 'trap "echo g3-exit" EXIT; trap - INT; kill -INT $$; echo g3-after'; echo "g3 $?"
trap - INT
for s in HUP USR1 USR2 ALRM ABRT PROF; do
  { "$BASH" -c 'trap "echo exit-'$s'" EXIT; kill -'$s' $$; echo after-'$s; } 2>/dev/null
  echo "$s $?"
done
"$BASH" -c 'trap "echo h1-exit st=\$?" EXIT; (exit 3); sleep 3; echo h1-after' & p=$!
sleep 1; kill -TERM $p; wait $p; echo "h1 $?"
"$BASH" -c 'trap "echo h2-exit" EXIT; while :; do :; done' & p=$!
sleep 1; kill -TERM $p; wait $p; echo "h2 $?"
sleep 3 | "$BASH" -c 'trap "echo h3-exit" EXIT; read x; echo h3-after' & p=$!
sleep 1; kill -TERM $p; wait $p; echo "h3 $?"
wait
{ "$BASH" -c 'trap "echo h8-exit" EXIT; trap "echo u" USR1; trap - USR1; kill -USR1 $$; echo h8-after'; } 2>/dev/null; echo "h8 $?"
{ "$BASH" -c 'trap "echo u" USR1; trap "echo h8b-exit" EXIT; trap - USR1; kill -USR1 $$; echo h8b-after'; } 2>/dev/null; echo "h8b $?"
"$BASH" -c 'trap "echo h9-exit >&2" EXIT; while :; do echo y; done' | head -1; echo "h9 ${PIPESTATUS[0]}"
{ "$BASH" -c 'trap "echo h12-exit" EXIT; trap - EXIT; kill -TERM $$; echo h12-after'; } 2>/dev/null; echo "h12 $?"
"$BASH" -c 'trap "echo h14-exit" EXIT; trap "echo h14-term; exit 7" TERM; kill -TERM $$; echo h14-after'; echo "h14 $?"
{ "$BASH" -c 'trap "echo h15-exit; kill -TERM \$\$; echo h15-exit-after" EXIT; kill -TERM $$; echo h15-after'; } 2>/dev/null; echo "h15 $?"
{ "$BASH" -c 'trap "echo x1-exit" EXIT; ( kill -TERM $BASHPID; echo x1-sub-alive ); echo "x1 sub=$?"'; } 2>/dev/null; echo "x1 $?"
{ "$BASH" -c 'trap "echo x2-exit" EXIT; ( trap "echo x2-inner" EXIT; kill -TERM $BASHPID; echo x2-sub-alive ); echo "x2 sub=$?"'; } 2>/dev/null; echo "x2 $?"
"$BASH" -c 'trap "echo x3-exit" EXIT; x=$(trap "echo x3-cs" EXIT; kill -TERM $BASHPID; echo alive); echo "x3 [$x] $?"'; echo "x3 $?"
{ "$BASH" -c 'trap "echo x4-exit" EXIT; f() { kill -TERM $$; echo x4-in-f; }; f; echo x4-after'; } 2>/dev/null; echo "x4 $?"
{ "$BASH" -c 'trap "echo x5-exit" EXIT; for i in 1 2; do kill -TERM $$; echo "x5-loop $i"; done'; } 2>/dev/null; echo "x5 $?"
{ "$BASH" -c 'trap "echo e1 st=\$?" EXIT; false; kill -TERM $$; echo e1-after'; } 2>/dev/null; echo "e1 $?"
{ "$BASH" -c 'trap "echo e3 st=\$?; exit 9" EXIT; kill -TERM $$'; } 2>/dev/null; echo "e3 $?"
{ "$BASH" -c 'f() { trap "echo e4 in-f" EXIT; kill -TERM $$; }; f; echo e4-after'; } 2>/dev/null; echo "e4 $?"
{ "$BASH" -c 'trap "echo e5 \$BASH_SUBSHELL" EXIT; echo $( kill -TERM $$; echo e5-cs ); echo e5-after'; } 2>/dev/null; echo "e5 $?"
{ trap 'echo h7-exit' EXIT; trap 'echo h7-got' INT; kill -INT $BASHPID; echo h7-after; } & wait
echo end
