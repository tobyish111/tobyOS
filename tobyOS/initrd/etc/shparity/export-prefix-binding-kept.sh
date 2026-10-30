# export, in a function, of a name its call's prefix assignment bound keeps
# the binding after the call -- `f() { export x; }; x=4 f` leaves x=4,
# exported, with what the function gave it (finding 381). tsh put the old x
# back.
exec 2>&1
f() { export x; }; x=4 f; echo "[$x]"; declare -p x
g() { y=5; export y; }; y=1; y=4 g; echo "[$y]"
h() { export z; }; k() { h; }; z=7 k; echo "[$z]"
m() { w=9; }; w=1; w=4 m; echo "[$w]"
