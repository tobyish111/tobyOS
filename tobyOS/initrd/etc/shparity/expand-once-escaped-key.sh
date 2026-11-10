# Under assoc_expand_once, arithmetic steps over an escaped bracket in a
# key -- `(( A[$k]=2 ))` with k=']' sets the key ] -- and read and printf -v
# of a word not written as an array reference still check it: `read
# "A[$k]"` with k=']' is "`A[]]': not a valid identifier" (finding 398).
exec 2>&1
shopt -s assoc_expand_once
declare -A A; for k in ']' '*' '@' ' '; do (( A[$k]=2 )); done; declare -p A
unset A; declare -A A; for k in ']' '*' '@'; do read "A[$k]" <<< X; done; declare -p A
for k in ']' '*'; do printf -v "A[$k]" %s Y; done; declare -p A
unset A; declare -A A; k=']'; read A[$k] <<< Z; declare -p A
