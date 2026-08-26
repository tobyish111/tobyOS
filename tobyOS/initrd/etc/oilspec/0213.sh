typeset -A assoc
assoc=([empty]='' [k]=v)

echo 'no quotes'

test -v assoc[empty]
echo empty=$?

test -v assoc[k]
echo k=$?

test -v assoc[nonexistent]
echo nonexistent=$?

echo

# Now with quotes
echo 'quotes'

test -v assoc["empty"]
echo empty=$?

test -v assoc['k']
echo k=$?

test -v assoc['nonexistent'] 
echo nonexistent=$?
