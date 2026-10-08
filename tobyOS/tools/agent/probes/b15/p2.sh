# p2: trapping or resetting a signal that is ignored -- in an async child
# without job control (bash's setup_async_signals ignores INT and QUIT), in
# a shell that was started with it ignored, and after the parent's trap ''
exec 2>&1
echo "== v1 async child traps INT"
{ trap 'echo v1-got-int' INT; kill -INT $BASHPID; echo v1-after; } &
wait; echo "v1 st=$?"
echo "== v2 async child resets INT and is killed by it"
{ trap - INT; kill -INT $BASHPID; echo v2-after; } &
wait; echo "v2 st=$?"
echo "== v3 a shell started with INT ignored"
trap '' INT
"$BASH" -c 'trap "echo v3-got-int" INT; kill -INT $$; echo v3-after; trap' ; echo "v3 st=$?"
"$BASH" -c 'trap - INT; kill -INT $$; echo v3b-after'; echo "v3b st=$?"
echo "== v4 the parent ignores INT; an async child traps it"
{ trap 'echo v4-got-int' INT; kill -INT $BASHPID; echo v4-after; trap; } &
wait; echo "v4 st=$?"
echo "== v5 a ( ) subshell of a shell ignoring INT traps it"
( trap 'echo v5-got-int' INT; kill -INT $BASHPID; echo v5-after )
echo "v5 st=$?"
trap - INT
echo "== v6 QUIT in an async child"
{ trap 'echo v6-got-quit' QUIT; kill -QUIT $BASHPID; echo v6-after; } &
wait; echo "v6 st=$?"
echo end
