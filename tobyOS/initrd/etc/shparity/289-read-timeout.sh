# `read -t` TIMES OUT, and for the whole line: 142 with what came assigned.
# On a pipe it never did -- in tsh, libtoby's poll() slept the timeout and
# said "ready", and the guest's bash saw the pipe as a regular file and
# turned its timeout off (logs/b15 rt, fst) -- and tsh waited for the line's
# end after its first byte.
exec 2>&1
sleep 3 | { read -t 1 x; echo "a $? [$x]"; }
read -t 1 x < <(sleep 3); echo "b $? [$x]"
{ printf ab; sleep 2; echo c; } | { read -t 1 x; echo "c $? [$x]"; }
{ echo line; sleep 2; } | { read -t 1 x; echo "d $? [$x]"; }
printf xy | { read -t 1 x; echo "e $? [$x]"; }
read -t 0 x < /dev/null; echo "f $?"
sleep 2 | { read -t 0 x; echo "g $?"; }
echo end
