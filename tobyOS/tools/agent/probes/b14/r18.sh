# r18: a descriptor the shell has closed (`exec 3>&-`) is closed in what it
# runs -- a spawned program and a background element that execs in place --
# and moving descriptors into place never clobbers one still to be moved
exec 2>&1
cd /tmp
rm -f r18a r18b
printf 'FROM-STDIN\n' > r18in
exec < r18in
exec 3>r18a; exec 3>&-
"$BASH" -c 'read -r l <&3 && echo "spawn: got [$l]" || echo "spawn: fd 3 closed"' 2>/dev/null
echo hi-pipe | cat > r18b &
wait
echo "pipe: [$(cat r18b)]"
exec 4>&-
echo two | cat | cat > r18b &
wait
echo "pipe2: [$(cat r18b)]"
exec 5</dev/null
echo three | cat > r18b &
wait
echo "pipe3: [$(cat r18b)]"
"$BASH" -c 'read -r l <&5; echo "fd5 open: $?"'
exec 5<&-
echo end
