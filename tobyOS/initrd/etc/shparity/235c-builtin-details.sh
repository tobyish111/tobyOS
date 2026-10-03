# Brace ranges across letters, readonly in posix mode, the ERR trap for a
# failing ( ), unset command names, alias -p/--, readonly -p/export -p.
# (batch 12; probe p44b in C:\t\b12)
exec 2>&1
readonly x=1
x=2; echo same-line
echo next-line
set -o posix
f() { x=3; echo in-f; }; f
echo after-f
