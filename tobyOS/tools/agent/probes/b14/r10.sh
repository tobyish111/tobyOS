exec 2>&1 </dev/null
# Which forked subshells run an EXIT trap they set (bash's subshell_exit
# against sh_exit): control structures in a foreground pipeline, async
# compounds, async and-or lists, functions.
if true; then trap 'echo IF' EXIT; echo a; fi | cat
for x in 1; do trap 'echo FOR' EXIT; echo b; done | cat
echo y | while read v; do trap 'echo WHILE' EXIT; echo "$v"; done
case x in x) trap 'echo CASE' EXIT; echo c ;; esac | cat
for x in 1; do trap 'echo AFOR' EXIT; echo d; done &
wait
if true; then trap 'echo AIF' EXIT; echo e; fi &
wait
true && { trap 'echo ANDOR' EXIT; echo f; } &
wait
{ trap 'echo GROUP' EXIT; echo g; } &
wait
g() { trap 'echo FUNC' EXIT; echo h; }
g &
wait
g | cat
echo y | g
( trap 'echo PAREN' EXIT; echo i ) &
wait
echo end
