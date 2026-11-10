# Brace ranges across letters, readonly in posix mode, the ERR trap for a
# failing ( ), unset command names, alias -p/--, readonly -p/export -p.
# (batch 12; probe p52 in C:\t\b12)
exec 2>&1
readonly -p rz0; echo "1 $?"; declare -p rz0
export -p rz0; echo "2 $?"; declare -p rz0
readonly -p ry=1; echo "3 $?"; declare -p ry
