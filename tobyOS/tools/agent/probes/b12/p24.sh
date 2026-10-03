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
