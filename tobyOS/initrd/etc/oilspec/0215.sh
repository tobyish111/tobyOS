typeset -A assoc
assoc=([empty]='' [k]=v)

echo 'no quotes'

[[ -v assoc[empty] ]]
echo empty=$?

[[ -v assoc[k] ]]
echo k=$?

[[ -v assoc[nonexistent] ]]
echo nonexistent=$?

echo

# Now with quotes
echo 'quotes'

[[ -v assoc["empty"] ]]
echo empty=$?

[[ -v assoc['k'] ]]
echo k=$?

[[ -v assoc['nonexistent'] ]]
echo nonexistent=$?

echo

echo 'vars'

key=empty
[[ -v assoc[$key] ]]
echo empty=$?

key=k
[[ -v assoc[$key] ]]
echo k=$?

key=nonexistent
[[ -v assoc[$key] ]]
echo nonexistent=$?
