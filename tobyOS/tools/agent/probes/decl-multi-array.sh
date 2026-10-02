# Finding 9: several array literals in one declaration. bash parses them
# (the word after an assignment builtin may be NAME=( ... )); tsh's own
# scanner called the second `(` a syntax error and ended the script.
exec 2>&1
declare -a q=(1) r=(9) t=(2)
declare -p q r t
local_test() { local -a a=(x y) b=(z); declare -p a b; }
local_test
export e1=(1) e2=(2) 2>/dev/null; echo "export: $?"
readonly ro1=(a) ro2=(b); declare -p ro1 ro2
typeset -A m=([k]=v) n=([j]=w); declare -p m n
echo done
