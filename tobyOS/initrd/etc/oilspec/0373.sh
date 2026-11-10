typeset -a a

test -v a
echo a=$?
test -v 'a[0]'
echo "a[0]=$?"
echo

a[0]=1

test -v a
echo a=$?
test -v 'a[0]'
echo "a[0]=$?"
echo

test -v 'a[1]'
echo "a[1]=$?"

# stupid rule about undefined 'x'
test -v 'a[x]'
echo "a[x]=$?"
echo
