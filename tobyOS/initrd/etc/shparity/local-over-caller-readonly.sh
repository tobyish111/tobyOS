# A function's local may shadow a readonly local its caller made: the new
# local is its own and writable, and the caller's is readonly again at the
# return; a plain assignment to the caller's is still refused (finding
# 379). tsh refused the local, "local: x: readonly variable".
exec 2>&1
a() { local x='1 2'; echo "a $x"; }; b() { local -r x=0; a; echo "b $x"; x=9; echo "not here"; }; b
echo "rc=$?"
c() { local x=(7 8); echo "c ${x[*]}"; }; d() { local -r x=0; c; }; d
e() { typeset x=5; echo "e $x"; }; g() { declare -r x=0; e; echo "g $x"; }; g
