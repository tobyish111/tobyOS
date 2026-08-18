typeset -A assoc
assoc=([empty]='' [k]=v)

[[ -v assoc[empty] ]]
echo empty=$?

[[ -v assoc[k] ]]
echo k=$?

[[ -v assoc[k]z ]]
echo typo=$?
