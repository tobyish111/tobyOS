# declare -n of a readonly variable is refused, "declare: RO: readonly
# variable", status 1, and an existing empty value is checked as a value
# given is (finding 334). tsh made the readonly variable a reference.
{
( declare -r RO=x; declare -n RO; echo "rc=$?"; declare -p RO; x=y; RO=z; echo "[$RO] [$x]" )
( r=""; declare -n r; echo "rc=$?"; declare -p r )
( f() { declare -r Q=1; local -n Q=z; echo "rc=$?"; declare -p Q; }; f )
} 2>&1
