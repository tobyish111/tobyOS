# In a command substitution fg and bg say "no current jobs", status 1,
# while jobs there lists the shell's -- tsh's fg printed the job and then
# "command not found", its bg "already in background" (finding 544).
exec 2>&1
set -m
sleep 20 &
echo "[$(jobs)]"
echo "[$(fg %% ; echo "rc=$?")]"
echo "[$(bg; echo "rc=$?")]"
exec 2>/dev/null
kill %1
wait
