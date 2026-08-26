typeset -A A

test -v A
echo A=$?
test -v 'A[0]'
echo "A[0]=$?"
echo

A['0']=x

test -v A
echo A=$?
test -v 'A[0]'
echo "A[0]=$?"
echo

test -v 'A[1]'
echo "A[1]=$?"

# stupid rule about undefined 'x'
test -v 'A[x]'
echo "A[x]=$?"
echo
