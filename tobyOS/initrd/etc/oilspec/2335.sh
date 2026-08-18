# NOTE: from spec/dbracket has a test case like this
# sane-array should turn this ON.
# bash and mksh allow this because of decay

a=('a b' 'c d')
b=('a' 'b' 'c' 'd')
echo ${#a[@]}
echo ${#b[@]}
[[ "${a[@]}" == "${b[@]}" ]] && echo EQUAL

shopt -s strict_array || true
[[ "${a[@]}" == "${b[@]}" ]] && echo EQUAL
