# A DESCRIPTOR THE SHELL HAS CLOSED IS CLOSED IN WHAT IT RUNS (batch 14),
# and putting descriptors in place never overwrites one still to be used.
# After `exec 3>&-` a spawned program's fd 3 was the shell's STDIN (the
# closed slot's handle read as descriptor 0); and the next pipe took the
# freed number 3, so a background pipeline's element that execs in place
# had its stdin copied from fd 3 after fd 3 had been replaced -- `echo hi
# | cat > f &` wrote the shell's stdin to f, and over SSH sat reading the
# session forever.
exec 2>&1
cd /tmp
rm -f fx279*
printf 'FROM-STDIN\n' > fx279in
exec < fx279in
exec 3>fx279a; exec 3>&-
"$BASH" -c 'read -r l <&3 && echo "spawn: got [$l]" || echo "spawn: fd 3 closed"' 2>/dev/null
echo hi-pipe | cat > fx279b &
wait
echo "pipe: [$(cat fx279b)]"
exec 4>&-
echo two | cat | cat > fx279b &
wait
echo "pipe2: [$(cat fx279b)]"
exec 5</dev/null
echo three | cat > fx279b &
wait
echo "pipe3: [$(cat fx279b)]"
"$BASH" -c 'read -r l <&5; echo "fd5 open: $?"'
exec 5<&-
exec < /dev/null
rm -f fx279*
echo end
