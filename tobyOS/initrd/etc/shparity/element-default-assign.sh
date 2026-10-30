# ${A[k]=v} stores v with the array's -i -l -u and gives the stored value,
# and ${A[@]:=v} of an associative array sets the key @ (finding 394). tsh
# stored and gave the word as written, and said "A[@]: bad array
# subscript".
exec 2>&1
declare -Au A; echo ${A[$(echo Darwin)]=foo}; declare -p A
declare -ai n; echo ${n[42]=4+3}; declare -p n
declare -A B; echo ${B[@]:=foo}; declare -p B
declare -Au C; echo ${C[@]:=bar}; declare -p C
