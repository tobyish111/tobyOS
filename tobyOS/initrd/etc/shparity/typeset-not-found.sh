# typeset -p of a name that is not set says "typeset: NAME: not found",
# named as it was called; tsh said "declare:" (finding 323).
{
typeset -p nosuchvar; echo "rc=$?"; declare -p nosuchvar; echo "rc=$?"
typeset -p PATH nosuch2 > /dev/null; echo "rc=$?"
} 2>&1
