# A raw ^A in a declaration's associative list is two, in a key and a
# value, unquoted or in double quotes -- not in single quotes, not in a
# list a plain assignment gives, not in an indexed one. tsh kept one
# (finding 568).
exec 2>&1
eval "declare -A A=( [$(printf '\001')]=$(printf '\001\001') )"; declare -p A
eval "declare -A B; B=( [$(printf '\001')]=$(printf '\001') )"; declare -p B
eval "declare -a I=( [0]=$(printf '\001') )"; declare -p I
eval "f() { local -A L=( [k]=\"$(printf '\001')\" [j]='$(printf '\001')' ); declare -p L; }"; f
eval "declare -A Q=( [a]=1 k=$(printf '\001') )"; declare -p Q
