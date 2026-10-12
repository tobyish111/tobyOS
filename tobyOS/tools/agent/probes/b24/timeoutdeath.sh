#!/bin/sh
# timeoutdeath.sh -- does the running shell see timeout DIE of a signal? GNU's
# timeout re-raises the signal that killed its command (not timed out), and
# a KILL sent to its own group takes it down too: the shell's job notice
# ("Terminated", "Killed") is the evidence. Run under bash or tsh; stderr is
# folded into stdout, the job notices' pids and line numbers left to the
# caller.
timeout 1 sh -c 'kill -TERM $$'; echo "selfterm=$?"
timeout 1 sh -c 'kill -INT $$'; echo "selfint=$?"
timeout 1 sh -c 'kill -USR1 $$'; echo "selfusr1=$?"
timeout -s KILL 0.3 sleep 2; echo "groupkill=$?"
timeout -f -s KILL 0.3 sleep 2; echo "fgkill=$?"
timeout 0.3 sleep 2; echo "fires=$?"
