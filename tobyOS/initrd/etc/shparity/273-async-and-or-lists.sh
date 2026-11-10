# AN AND-OR LIST ENDED BY `&` IS ONE ASYNCHRONOUS LIST, and an async
# subshell runs its own EXIT trap as it ends (batch 14). bash's grammar has
# `&` and `;` separate and-or lists: `sleep 1 && echo x &` backgrounds the
# sleep AND the echo. tsh split on `&&` first, so the sleep ran in front and
# this script's "front" line came out after the background's. And an async
# `{ ...; }` or function that set an EXIT trap never ran it.
sleep 1 && echo "and-or list done" &
echo "front: the list is behind"
wait
false || sleep 1 | cat && echo "or-and list done" &
echo "front again"
wait
{ trap 'echo group exit trap' EXIT; echo in group; } &
wait
( trap 'echo subshell exit trap' EXIT; echo in subshell ) &
wait
f() { trap 'echo function exit trap' EXIT; echo in function; }
f &
wait
# the parent's caught traps are not the child's
trap 'echo parent exit trap' EXIT
{ echo child without a trap; } &
wait
echo end
