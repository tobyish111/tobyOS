# declare of a value for a readonly name says so under the builtin's name,
# "declare: x: readonly variable" (finding 347); a compound value keeps its
# own words. tsh said the bare message.
{
( readonly x=1; declare x=2; echo "rc=$?"; typeset x=3; echo "rc=$?"; f() { declare -g x=4; }; f; echo "rc=$?" )
} 2>&1
