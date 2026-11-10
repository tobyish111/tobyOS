# declare of a value through a reference to a readonly name names the
# REFERENCE in its message, as it was written: "declare: r: readonly
# variable" -- a chain, -i and += too (finding 361). tsh named the target.
exec 2>&1
readonly t=1; declare -n r=t
declare r=2; echo "rc=$? [$t]"
declare -i r=3; echo "rc=$? [$t]"
declare -n s=r; declare s=5; echo "rc=$? [$t]"
declare r+=8; echo "rc=$? [$t]"
