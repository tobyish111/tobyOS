# A for loop's list is words to its `;` or newline, reserved words or not:
# `for for in for; do` runs once -- tsh took the list's `for` for a loop of
# its own and found no `do` (finding 444).
exec 2>&1
for for in for; do echo in; echo done; done
for do in do; do echo "do=$do"; done
for x in if then while case; do echo $x; done
for y in "a;b" $(echo c) ; do echo "$y"; done
