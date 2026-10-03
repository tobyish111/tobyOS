# Brace ranges across letters, readonly in posix mode, the ERR trap for a
# failing ( ), unset command names, alias -p/--, readonly -p/export -p.
# (batch 12; probe p44 in C:\t\b12)
exec 2>&1
set -o posix
readonly x=1
x=2
echo hi
