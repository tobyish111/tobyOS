# declare +n with other attributes or a value: +n comes off the reference
# and the rest goes to what it named; +n alone does not make the target
# (finding 328). tsh put -i on the reference and made the target.
{
unset bar; declare -n foo=bar; declare +n foo; declare -p foo bar
declare -n foo; declare +n -i foo=7+4; declare -p foo bar
declare -n p=q; declare +n -x p; declare -p p q
declare -n s=t; declare +n s=v; declare -p s t
} 2>&1
