# A reference with no target takes a value as its target, and the value
# must name a variable: an assignment of 12345 is "`12345': not a valid
# identifier" and discards the line; declare of it at the top removes the
# reference, in the function that made it keeps it; `local -n l` with no
# value is a reference with no target (finding 327). tsh stored 12345.
declare -n r
{ r=12345; echo "same line"; } 2>&1
echo "next rc=$?"; declare -p r
r=x; echo "rc=$?"; declare -p r
declare -n s
{ declare s=12345; echo "rc=$?"; } 2>&1
{ declare -p s; echo "rc=$?"; } 2>&1
f() { typeset -n foo; typeset foo=12345; typeset -p foo; }; { f; } 2>&1
g() { local -n l; declare -p l; l=x; declare -p l; }; g
h() { local -n m; m=99; echo "not here"; }; { h; echo "nor here"; } 2>&1
echo "after rc=$?"
