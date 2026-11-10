# break and continue with a count of 0 or less say "loop count out of
# range", status 1, and leave every loop of that level -- a function's loops,
# not its caller's; a count that is not a number still ends the script
# (finding 412).
exec 2>&1
for z in 1 2 3; do echo "in $z"; break 0; echo "after $z"; done; echo "rc=$?"
for z in 1 2 3; do echo "in $z"; continue 0; echo "after $z"; done; echo "rc=$?"
for a in 1 2; do for b in 1 2; do echo "$a$b"; continue -1; echo no; done; echo "outer $a"; done; echo "rc=$?"
for z in 1 2; do break 0 || echo "st=$?"; echo after; done; echo "rc=$?"
f() { for z in 1 2; do break 0; echo no; done; echo "f rc=$?"; }; for y in 1 2; do f; echo "y $y"; done
