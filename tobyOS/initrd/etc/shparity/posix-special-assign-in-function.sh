# In POSIX mode a special builtin's assignment inside a function outlives
# the call that gave its name a prefix binding: `g() { var=20 return 5; };
# var=31 g` leaves var 20, where a plain assignment in the function is
# undone with the binding -- tsh undid both (finding 507).
exec 2>&1
set -o posix
var=0
f() { echo "in $var"; }
var=30 f
echo "after $var"
g() { var=20 return 5; }
var=31 g
echo "$? $var"
h() { var=22; }
var=32 h
echo "after h $var"
set +o posix
var=0
var=33 g
echo "$? $var"
