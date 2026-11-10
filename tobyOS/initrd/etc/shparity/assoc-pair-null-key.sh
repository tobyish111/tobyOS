# A null key in the pair form is bash's "WORD: bad array subscript", the
# word as it was written -- '' when the list is a declaration's argument --
# and its pair is dropped (finding 363). tsh stored the key "".
exec 2>&1
declare -A A; A=( "" null ); echo "rc=$?"; declare -p A
declare -A B; B=( a 1 "" 2 b 3 ); declare -p B
e=; declare -A C; C=( "$e" v ); declare -p C
declare -A D=( '' v4 ); declare -p D
