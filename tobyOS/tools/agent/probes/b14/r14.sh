# r14: an async command's redirections and the EXIT trap its subshell runs
exec 2>&1
cd /tmp
rm -f r14o*
f() { trap 'echo bye-f' EXIT; echo hi-f; }
f > r14o1 &
wait; echo "f:"; cat r14o1
{ trap 'echo bye-g' EXIT; echo hi-g; } > r14o2 &
wait; echo "group:"; cat r14o2
{ trap 'echo bye-g2' EXIT; echo hi-g2; } > r14o3 2> r14o3e &
wait; echo "group2:"; cat r14o3
trap 'echo bye-s' EXIT > r14o4 &
wait; echo "trapcmd:"; cat r14o4
{ trap 'echo bye-p' EXIT; echo hi-p; } > r14o5 | cat &
wait; echo "pipe:"; cat r14o5
if true; then trap 'echo bye-if' EXIT; echo hi-if; fi > r14o6 &
wait; echo "if:"; cat r14o6
{ trap 'echo bye-n' EXIT; echo hi-n; } &
wait
f &
wait
echo end
