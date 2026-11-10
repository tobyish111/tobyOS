# A reference's value that names an element of the reference itself is a
# self reference, as its own name is: refused at the top level, "nameref
# variable self references not allowed", warned of in a function; and a
# function's self reference over a readonly global stops at its readonly
# error (finding 356). tsh made the top-level one.
exec 2>&1
declare -n a='a[0]'; echo "rc=$?"; declare -p a
f() { local -n b='b[1]'; echo "in rc=$?"; }; f
readonly r=ro; g() { typeset -n r=r; echo "rc=$?"; }; g
