# $LINENO inside a multi-line $( ) counts from the command's own line, and
# blank and comment lines are not counted (bash runs the printed text).
# (batch 12; probe p24 in C:\t\b12)
exec 2>&1
a=(1)
echo "A1 $(echo $LINENO
echo $LINENO)"
echo "A2 $(echo $LINENO
echo $LINENO)" | cat
echo "A3 $(echo $LINENO
echo ${a[]})"
echo "A4 $(echo $LINENO
echo ${a[]})" | cat
echo A5 "$(echo $LINENO
echo $LINENO)"
echo "A6 $(echo $LINENO
echo $LINENO)"; echo "A6b $LINENO"
echo A7 $(echo $LINENO
echo ${a[]})
echo "A8 $(echo $LINENO
echo $LINENO)" > /dev/stdout
