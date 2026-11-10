# r13: what the kernel shell's /bin/tsh async child must match (finding 63)
exec 2>&1
cd /tmp
rm -f r13o1 r13o2
# where an async group's EXIT trap writes: inside or outside its redirection
{ trap 'echo bye' EXIT; echo hi; } > r13o1 &
wait
echo "file:"; cat r13o1
( trap 'echo bye2' EXIT; echo hi2 ) > r13o2 &
wait
echo "file2:"; cat r13o2
# $_ after an async function call
f() { :; }
echo a b; f x y > /dev/null &
echo "lastarg=[$_]"
wait
echo a b; sleep 0 &
echo "lastarg2=[$_]"
wait
# $! of a finished job is still waitable, with its status
g() { return 7; }
g &
sleep 1
wait $!; echo "wait-finished=$?"
# kill %N of an async group under job control reaches what it runs
set -m
{ sleep 30; echo never; } &
sleep 1
kill %1; wait %1; echo "killed=$?"
sleep 1
ps -o pid,args 2>/dev/null | grep -c "[s]leep 30" || ps | grep -c "[s]leep 30"
echo end
