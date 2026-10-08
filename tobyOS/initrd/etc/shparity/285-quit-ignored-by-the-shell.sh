# EVERY bash IGNORES SIGQUIT (sig.c initialize_shell_signals), and `trap -
# QUIT` says so again; what it runs, and its subshells and $( ), get the
# default back. tsh died of `kill -QUIT $$`, 131 (logs/b15 p5 q1-q5).
exec 2>&1
"$BASH" -c 'kill -QUIT $$; echo q1-alive'; echo "q1 $?"
"$BASH" -c 'sed -n "s/^SigIgn:[[:space:]]*/q2 /p" /proc/self/status; kill -QUIT $$; echo q2-alive'; echo "q2 $?"
"$BASH" -c '{ ( kill -QUIT $BASHPID; echo q3-sub-alive ); } 2>/dev/null; echo "q3 sub=$?"'; echo "q3 $?"
"$BASH" -c 'x=$(kill -QUIT $BASHPID; echo q4-cs-alive); echo "[$x]"'; echo "q4 $?"
"$BASH" -c 'trap "echo q5-trap" QUIT; kill -QUIT $$; echo q5-alive; trap - QUIT; kill -QUIT $$; echo q5-still'; echo "q5 $?"
echo end
