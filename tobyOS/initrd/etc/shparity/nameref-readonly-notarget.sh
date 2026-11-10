# declare -r of a reference with no target makes the reference itself
# readonly -- the readonly builtin says "`'" instead -- and a value for it
# is then refused (finding 348).
{
( typeset -n f1; typeset -r f1; echo "rc=$?"; typeset -p f1; typeset f1=bar; echo "rc=$?"; typeset -p f1 )
( declare -n z; readonly z; echo "rc=$?"; declare -p z )
} 2>&1
