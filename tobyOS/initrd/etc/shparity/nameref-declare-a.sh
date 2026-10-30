# declare -a or -A of a reference with no target makes it the array, quietly
# (finding 340). tsh kept the reference, and the array never showed.
{
( declare -n foo; declare -a foo; declare -p foo; foo[0]=7; declare -p foo )
( declare -n bar; declare -A bar; bar[k]=v; declare -p bar )
} 2>&1
