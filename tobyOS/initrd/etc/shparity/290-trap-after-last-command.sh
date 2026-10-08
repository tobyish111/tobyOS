# A TRAPPED SIGNAL THAT CAME WITH THE LAST COMMAND IS STILL TAKEN: bash runs
# pending traps after every command, the last included, and a trap's `exit`
# is the status. tsh looked for them only before a next command, so a -c
# string's last command left them unrun -- 0 and no output (logs/b15 p6
# i4).
exec 2>&1
"$BASH" -c 'trap "echo t1" USR1; kill -USR1 $$'; echo "t1 $?"
"$BASH" -c 'trap "echo t2; exit 4" USR1; kill -USR1 $$'; echo "t2 $?"
"$BASH" -c 'trap "echo t3; exit 5" USR1; ( sleep 1; kill -USR1 $$ ) & sleep 3 & wait $!'; echo "t3 $?"
"$BASH" -c 'trap "echo t4" USR1; f() { kill -USR1 $$; }; f'; echo "t4 $?"
echo end
