# A SIGNAL THE PARENT CAUGHT IS NOT CAUGHT IN ITS SUBSHELL (bash's
# restore_original_signals): the trap is on show there, not in force, and
# the signal takes its default -- in a ( ), a $( ), a pipeline element. tsh's
# children kept the parent's handler and swallowed it: "alive", 0
# (logs/b15 p7 s1-s6).
exec 2>&1
trap 'echo caught' USR1
{ ( kill -USR1 $BASHPID; echo s1-alive ); } 2>/dev/null; echo "s1 $?"
x=$(kill -USR1 $BASHPID; echo s2-alive); echo "s2 [$x] $?"
{ kill -USR1 $BASHPID; echo s3-alive; } & wait $!; echo "s3 $?"
kill -USR1 $BASHPID | cat; echo "s4 ${PIPESTATUS[0]}"
{ ( trap - USR1; kill -USR1 $BASHPID; echo s5-alive ); } 2>/dev/null; echo "s5 $?"
( trap; trap 'echo s6-own' USR1; kill -USR1 $BASHPID; echo s6-alive ); echo "s6 $?"
kill -USR1 $$; echo "s7 $?"
trap - USR1
echo end
