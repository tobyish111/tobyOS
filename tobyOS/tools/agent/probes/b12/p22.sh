exec 2>&1
echo "1 $(echo $LINENO)"
x=$(echo $LINENO
echo $LINENO
echo $LINENO); echo "2 [$x]" | tr '\n' ' '; echo
y=$(
echo $LINENO
); echo "3 [$y]"
z=`echo $LINENO
echo $LINENO`; echo "4 [$z]" | tr '\n' ' '; echo
echo "5 $(echo a
echo ${a[]})"
f() {
  echo "6 $(echo $LINENO
echo $LINENO)" | tr '\n' ' '; echo
}
f
echo "7 $( (echo $LINENO
echo $LINENO) )" | tr '\n' ' '; echo
w=$(cat <<E
$LINENO
E
); echo "8 [$w]"
echo "9 $LINENO"
