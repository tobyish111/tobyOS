# p3: signals ignored at entry and what children inherit (finding 72).
# SigIgn bits: HUP 0x1, INT 0x2, QUIT 0x4, USR1 0x200, TERM 0x4000.
exec 2>&1
echo "== a: what a command inherits in an async child"
sed -n 's/^SigIgn:[[:space:]]*/a1-fg /p' /proc/self/status
sed -n 's/^SigIgn:[[:space:]]*/a2-async /p' /proc/self/status & wait
{ sed -n 's/^SigIgn:[[:space:]]*/a3-brace-last /p' /proc/self/status; } & wait
{ sed -n 's/^SigIgn:[[:space:]]*/a4-brace-mid /p' /proc/self/status; :; } & wait
( sed -n 's/^SigIgn:[[:space:]]*/a5-paren /p' /proc/self/status ) & wait
sed -n 's/^SigIgn:[[:space:]]*/a6-pipe /p' /proc/self/status | cat & wait
{ trap - INT; sed -n 's/^SigIgn:[[:space:]]*/a7-revert-int /p' /proc/self/status; } & wait
{ trap - QUIT; sed -n 's/^SigIgn:[[:space:]]*/a8-revert-quit /p' /proc/self/status; } & wait
{ trap 'echo x' INT; sed -n 's/^SigIgn:[[:space:]]*/a9-trapped-int /p' /proc/self/status; } & wait
{ trap - INT; trap -p INT; kill -INT $BASHPID; echo a10-after; } & wait
echo "a10 st=$?"
echo "== b: a shell started with USR1 ignored"
trap '' USR1
"$BASH" -c 'trap "echo b1-got" USR1; echo "b1 -p:"; trap -p USR1; echo "b1 all:"; trap; kill -USR1 $$; echo b1-after'; echo "b1 st=$?"
"$BASH" -c 'trap - USR1; trap -p USR1; kill -USR1 $$; echo b2-after'; echo "b2 st=$?"
"$BASH" -c 'trap "" USR1; trap -p; sed -n "s/^SigIgn:[[:space:]]*/b3 /p" /proc/self/status'; echo "b3 st=$?"
"$BASH" -c '( trap "echo b4-got" USR1; kill -USR1 $BASHPID; echo b4-after )'; echo "b4 st=$?"
"$BASH" -c 'trap USR1; trap -p USR1; kill -USR1 $$; echo b5-after'; echo "b5 st=$?"
trap - USR1
echo "== c: a shell started with INT ignored"
trap '' INT
"$BASH" -c 'trap - INT; trap; sed -n "s/^SigIgn:[[:space:]]*/c1 /p" /proc/self/status; kill -INT $$; echo c1-after'; echo "c1 st=$?"
"$BASH" -c 'trap "echo c2" INT; trap - INT; trap -p INT; kill -INT $$; echo c2-after'; echo "c2 st=$?"
"$BASH" -c 'trap "" INT; trap -p INT; kill -INT $$; echo c3-after'; echo "c3 st=$?"
trap - INT
echo "== d: QUIT"
"$BASH" -c 'trap - QUIT; trap; sed -n "s/^SigIgn:[[:space:]]*/d1 /p" /proc/self/status; kill -QUIT $$; echo d1-after'; echo "d1 st=$?"
"$BASH" -c 'trap "echo d2" QUIT; trap - QUIT; trap -p QUIT; kill -QUIT $$; echo d2-after'; echo "d2 st=$?"
trap '' QUIT
"$BASH" -c 'trap "echo d3" QUIT; trap; kill -QUIT $$; echo d3-after'; echo "d3 st=$?"
trap - QUIT
echo "== e: TERM and HUP"
trap '' TERM HUP
"$BASH" -c 'trap "echo e1" TERM HUP; trap; kill -TERM $$; kill -HUP $$; echo e1-after'; echo "e1 st=$?"
trap - TERM HUP
echo "== g: the EXIT trap and a fatal signal"
"$BASH" -c 'trap "echo g1-exit" EXIT; kill -TERM $$; echo g1-after'; echo "g1 st=$?"
"$BASH" -c 'trap "echo g2-exit" EXIT; kill -INT $$; echo g2-after'; echo "g2 st=$?"
trap '' INT
"$BASH" -c 'trap "echo g3-exit" EXIT; trap - INT; kill -INT $$; echo g3-after'; echo "g3 st=$?"
trap - INT
echo end
