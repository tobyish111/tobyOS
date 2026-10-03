exec 2>&1
a=(1)
cat <<E
ok $LINENO
E
echo "a $LINENO"
cat <<E
${a[]}
E
echo "b $LINENO"
cat <<E; echo "c $LINENO"
x
E
echo "d $LINENO"
cat <<E; echo "e $LINENO"
${a[]}
E
echo "f $LINENO"
f() {
  cat <<E
${a[]} $LINENO
E
  echo "g $LINENO"
}
f
echo "h $LINENO"
cat <<E1 <<E2
${a[]}
E1
two
E2
echo "i $LINENO"
