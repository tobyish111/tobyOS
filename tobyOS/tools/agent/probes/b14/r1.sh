exec 2>&1 </dev/null
# an and-or list with & is ONE async list in bash: nothing of it runs in front
s=$SECONDS
sleep 1 && echo "and-or done" &
echo "after & : $((SECONDS - s))s"
wait
false || sleep 1 | cat &
echo "after || | & : $((SECONDS - s))s"
wait
# the EXIT trap of an async group / subshell runs when it ends
{ trap 'echo group-exit' EXIT; echo in-group; } &
wait
( trap 'echo sub-exit' EXIT; echo in-sub ) &
wait
f() { trap 'echo func-exit' EXIT; echo in-func; }
f &
wait
# expansions of an async command happen in the child
unset bar
echo ${bar=2} >/dev/null &
wait
echo "bar=${bar-unset}"
echo end
