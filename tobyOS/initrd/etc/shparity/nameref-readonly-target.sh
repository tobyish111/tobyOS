# readonly and declare -r of a reference make the variable at the end of
# its chain readonly, not the reference; a reference to an element or to
# nothing is "`NAME': not a valid identifier"; readonly -n is taken and
# marks nothing (finding 320). tsh marked the reference.
{
( declare -n r=t; readonly r; t=4; echo "rc=$? [$t] [$r]"; declare -p r t )
( declare -n r=t; readonly r=5; declare -p r t )
( declare -n r=t; declare -r r; declare -p r t; declare -n s=w; declare -r s=9; declare -p s w )
( t=1; declare -n r=t; declare -n s=r; readonly s; declare -p r s t )
( a=(1 2); declare -n e='a[1]'; readonly e; echo "rc=$?"; declare -p e; declare -n z; readonly z; echo "rc=$?" )
( declare -n q=u; readonly -n q; echo "rc=$?"; q=x; echo "rc=$? [$u]"; readonly -n v=1; declare -p v )
} 2>&1
