# A FORKED CHILD RUNS ITS EXIT TRAP WITH ITS COMMAND'S REDIRECTIONS IN
# FORCE (batch 14), as bash's subshell does: it applies them for good, not
# undoably, so the trap of `f > out &` -- a function, a builtin or a { }
# group -- writes into out. tsh put its descriptors back as the command
# returned and the trap wrote to the child's stdout. An if, a loop and an
# async pipeline's { } element run no EXIT trap at all.
exec 2>&1
cd /tmp
rm -f fx278o*
f() { trap 'echo bye-f' EXIT; echo hi-f; }
f > fx278o1 &
wait; echo "f:"; cat fx278o1
{ trap 'echo bye-g' EXIT; echo hi-g; } > fx278o2 &
wait; echo "group:"; cat fx278o2
{ trap 'echo bye-g2' EXIT; echo hi-g2; } > fx278o3 2> fx278o3e &
wait; echo "group2:"; cat fx278o3
trap 'echo bye-s' EXIT > fx278o4 &
wait; echo "builtin:"; cat fx278o4
( trap 'echo bye-sub' EXIT; echo hi-sub ) > fx278o5 &
wait; echo "subshell:"; cat fx278o5
{ trap 'echo bye-p' EXIT; echo hi-p; } > fx278o6 | cat &
wait; echo "pipe:"; cat fx278o6
if true; then trap 'echo bye-if' EXIT; echo hi-if; fi > fx278o7 &
wait; echo "if:"; cat fx278o7
{ trap 'echo bye-fg' EXIT; echo hi-fg; } > fx278o8 | cat
echo "fg stage:"; cat fx278o8
{ trap 'echo bye-n' EXIT; echo hi-n; } &
wait
rm -f fx278o*
echo end
