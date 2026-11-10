exec 2>&1 </dev/null
# Which pipeline elements run an EXIT trap they set: a { } or a ( ),
# first or last, in front or in the background.
{ trap 'echo T1' EXIT; echo a; } | cat
{ trap 'echo T2' EXIT; echo b; } | cat &
wait
echo x | { trap 'echo T3' EXIT; cat; }
echo y | { trap 'echo T4' EXIT; cat; } &
wait
( trap 'echo T5' EXIT; echo e ) | cat
( trap 'echo T6' EXIT; echo f ) | cat &
wait
echo z | ( trap 'echo T7' EXIT; cat )
f() { trap 'echo T8' EXIT; echo g; }
f | cat
f | cat &
wait
{ trap 'echo T9' EXIT; echo h; } &
wait
echo end
