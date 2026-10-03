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
