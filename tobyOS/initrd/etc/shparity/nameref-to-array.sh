# An array made under a reference with no target -- r=(...), mapfile, read
# -a, coproc -- removes the reference, "warning: r: removing nameref
# attribute"; an element of one is "`': not a valid identifier" (finding
# 335). tsh kept -n over the array, and made an empty element 0.
{
( declare -n r; r=(1 2); echo "rc=$?"; declare -p r )
( declare -n s; mapfile s < /dev/null; declare -p s; declare -n t; read -a t <<< 'p q'; declare -p t )
( declare -n c; coproc c { :; }; echo "[${#c[@]}]"; wait )
} 2>&1
declare -n e
{ e[1]=z; echo "same line"; } 2>&1
echo "next $?"
