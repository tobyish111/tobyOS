# declare -A of an indexed array only declared, with no element, is
# refused as of one with elements: "cannot convert indexed to associative
# array", status 1, the array still -a (finding 373). tsh converted it.
exec 2>&1
f() { declare -a a; declare -A a; echo "rc=$?"; declare -p a; }; f
declare -a b; declare -A b; echo "rc=$?"; declare -p b
