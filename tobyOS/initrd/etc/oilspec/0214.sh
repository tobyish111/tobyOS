typeset -A assoc
assoc=([empty]='' [k]=v)

key=empty
test -v 'assoc[$key]'
echo empty=$?

key=k
test -v 'assoc[$key]'
echo k=$?

key=nonexistent
test -v 'assoc[$key]'
echo nonexistent=$?
