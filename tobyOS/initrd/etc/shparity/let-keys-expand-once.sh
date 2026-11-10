# Under assoc_expand_once, let's associative keys are the text as it
# stands in let's expression, quotes and all; (( )) and $(( )) take the
# quotes off. tsh took them off in let too (finding 572).
exec 2>&1
shopt -s assoc_expand_once
declare -A a; v=k
let 'a[" "]=1' 'a[$v]=2' "a[\"$v\"]=3"
declare -p a
(( a[" "]=4 )); echo $(( a['q']=5 ))
declare -p a
