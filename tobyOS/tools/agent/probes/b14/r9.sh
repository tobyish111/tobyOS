exec 2>&1 </dev/null
# When does an async pipeline element run the EXIT trap it sets? (r8: the
# first element's ran, the last element's `{ trap; cat; }` did not.)
echo y | { trap 'echo L-builtin' EXIT; echo inner; } &
wait
echo y | { trap 'echo L-external' EXIT; cat; } &
wait
echo y | ( trap 'echo L-paren' EXIT; cat ) &
wait
f2() { trap 'echo L-func' EXIT; cat; }
echo y | f2 &
wait
echo y | { trap 'echo M-external' EXIT; cat; } | cat &
wait
echo y | { trap 'echo M-builtin' EXIT; read v; echo "m $v"; } | cat &
wait
{ trap 'echo F-external' EXIT; cat /dev/null; } | cat &
wait
{ trap 'echo F-builtin' EXIT; echo first; } | cat &
wait
echo y | { trap 'echo FG-L-external' EXIT; cat; }
echo end
