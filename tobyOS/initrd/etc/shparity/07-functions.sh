# Functions: argument passing, $#, return codes, and calling a function that
# is defined LATER in the file (legal, because definition happens at run time,
# not parse time).
greet() { echo "hello $1"; }
greet world
greet "two words"

count() { echo "args=$# all=[$*]"; }
count a b c
count

status() { return 7; }
status
echo "rc=$?"

outer() { inner; }
inner() { echo nested; }
outer

shadow() { echo "fn:$1"; }
x=outerval
usesglobal() { echo "sees:$x"; }
usesglobal
shadow arg
echo funcs-done
