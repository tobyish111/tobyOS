# Brace ranges across letters, readonly in posix mode, the ERR trap for a
# failing ( ), unset command names, alias -p/--, readonly -p/export -p.
# (batch 12; probe p46 in C:\t\b12)
exec 2>&1
$NOPE -c '
a
b
' 2>&1 | cat
$NOPE -c '
a
'
echo "L $LINENO"
nope_cmd 'x
y'
echo "L2 $LINENO 'q
z'"
