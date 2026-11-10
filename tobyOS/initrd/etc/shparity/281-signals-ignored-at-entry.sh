# A SIGNAL IGNORED WHEN THE SHELL STARTED can be neither trapped nor reset,
# and nothing is said (POSIX 2.14 trap; bash's SIG_HARD_IGNORE): `trap
# CMD`, `trap ''` and `trap -` change nothing, the listing shows `trap --
# '' SIGX`, and a subshell is the same -- bar bash's `trap - INT`, after
# which INT ends the shell. And `trap - QUIT` leaves QUIT ignored: every
# bash ignores it. tsh trapped them all (logs/b15 p3, finding 72).
exec 2>&1
trap '' USR1
"$BASH" -c 'trap "echo b1-got" USR1; trap -p USR1; trap; kill -USR1 $$; echo b1-after'; echo "b1 $?"
"$BASH" -c 'trap - USR1; trap -p USR1; kill -USR1 $$; echo b2-after'; echo "b2 $?"
"$BASH" -c 'trap "" USR1; trap -p'; echo "b3 $?"
"$BASH" -c '( trap "echo b4-got" USR1; kill -USR1 $BASHPID; echo b4-after )'; echo "b4 $?"
"$BASH" -c 'trap USR1; trap -p USR1'; echo "b5 $?"
trap - USR1
trap '' INT
"$BASH" -c 'trap - INT; trap; kill -INT $$; echo c1-after'; echo "c1 $?"
"$BASH" -c 'trap "echo c2" INT; trap - INT; trap -p INT; kill -INT $$; echo c2-after'; echo "c2 $?"
"$BASH" -c 'trap "" INT; trap -p INT; kill -INT $$; echo c3-after'; echo "c3 $?"
trap - INT
"$BASH" -c 'trap - QUIT; trap; kill -QUIT $$; echo d1-after'; echo "d1 $?"
"$BASH" -c 'trap "echo d2" QUIT; trap - QUIT; trap -p QUIT; kill -QUIT $$; echo d2-after'; echo "d2 $?"
trap '' QUIT
"$BASH" -c 'trap "echo d3" QUIT; trap; kill -QUIT $$; echo d3-after'; echo "d3 $?"
trap - QUIT
trap '' TERM HUP
"$BASH" -c 'trap "echo e1" TERM HUP; trap; kill -TERM $$; kill -HUP $$; echo e1-after'; echo "e1 $?"
trap - TERM HUP
echo end
