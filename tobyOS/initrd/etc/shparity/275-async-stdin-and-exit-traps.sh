# AN ASYNCHRONOUS COMMAND READS /dev/null, AND WHICH FORKED CHILDREN RUN
# THEIR EXIT TRAP (batch 14), as bash does both. Without job control, or
# in a subshell, an async command's stdin is /dev/null -- unless it
# redirects stdin itself, or a control structure around it ran with a
# stdin redirection (bash's stdin_redir, which stays set), or it reads a
# pipe; an async PIPELINE keeps stdin. A forked child runs the EXIT trap
# it set when it is a simple command, a ( ), or a { } that is async on its
# own or a stage of a foreground pipeline -- not an if, a loop, an and-or
# list, or a { } stage of an async pipeline. tsh gave every async command
# the shell's stdin, and ran the trap for every async child and for no
# pipeline stage.
exec 2>&1
printf 'L\n' | "$BASH" -c 'cat & wait; echo e1'
printf 'L\n' | "$BASH" -c 'cat | cat & wait; echo e2'
printf 'L\n' | "$BASH" -c '{ cat; } & wait; echo e3'
printf 'L\n' | "$BASH" -c 'cat 0<&0 & wait; echo e4'
printf 'L\n' | "$BASH" -c 'while false; do :; done < /dev/null; cat & wait; echo e5'
printf 'L\n' | "$BASH" -c '{ cat & wait; } 0<&0; echo e6'
printf 'L\n' | "$BASH" -c 'echo x | { cat & wait; }; echo e7'
printf 'L\n' | "$BASH" -c '( cat & wait ); echo e8'
printf 'L\n' | "$BASH" -c 'f() { cat & wait; }; f; echo e9'
printf 'L\n' | "$BASH" -c 'x=$(cat & wait); echo "e10 [$x]"'
printf 'L\n' | "$BASH" -c 'set -m; cat & wait; echo e11' 2>/dev/null
{ trap 'echo T1' EXIT; echo a; } | cat
{ trap 'echo T2' EXIT; echo b; } | cat &
wait
echo x | { trap 'echo T3' EXIT; cat; }
echo y | { trap 'echo T4' EXIT; cat; } &
wait
( trap 'echo T5' EXIT; echo e ) | cat &
wait
f() { trap 'echo T6' EXIT; echo g; }
f | cat
echo y | f
f | cat &
wait
f &
wait
if true; then trap 'echo T7' EXIT; echo h; fi | cat
for x in 1; do trap 'echo T8' EXIT; echo i; done &
wait
true && { trap 'echo T9' EXIT; echo j; } &
wait
{ trap 'echo T10' EXIT; echo k; } &
wait
echo end
