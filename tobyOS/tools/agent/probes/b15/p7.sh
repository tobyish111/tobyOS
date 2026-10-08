# p7: a signal the parent CAUGHT, in its subshells; and $? in the EXIT
# trap a fatal signal runs.
exec 2>&1
trap 'echo caught' USR1
( kill -USR1 $BASHPID; echo s1-alive ); echo "s1 $?"
x=$(kill -USR1 $BASHPID; echo s2-alive); echo "s2 [$x] $?"
{ kill -USR1 $BASHPID; echo s3-alive; } & wait $!; echo "s3 $?"
kill -USR1 $BASHPID | cat; echo "s4 ${PIPESTATUS[0]}"
( trap - USR1; kill -USR1 $BASHPID; echo s5-alive ); echo "s5 $?"
( trap; trap 'echo s6-own' USR1; kill -USR1 $BASHPID; echo s6-alive ); echo "s6 $?"
trap - USR1
"$BASH" -c 'trap "echo e1 st=\$?" EXIT; false; kill -TERM $$; echo e1-after'; echo "e1 $?"
"$BASH" -c 'trap "echo e2 st=\$?" EXIT; (exit 3); sleep 3; echo e2-after' & p=$!
sleep 1; kill -TERM $p; wait $p; echo "e2 $?"
"$BASH" -c 'trap "echo e3 st=\$?; exit 9" EXIT; kill -TERM $$'; echo "e3 $?"
"$BASH" -c 'f() { trap "echo e4 in-f" EXIT; kill -TERM $$; }; f; echo e4-after'; echo "e4 $?"
"$BASH" -c 'trap "echo e5 \$BASH_SUBSHELL" EXIT; echo $( kill -TERM $$; echo e5-cs ); echo e5-after'; echo "e5 $?"
echo end
