a=('1 2' '3 4')
s='1 2 3 4'  # length 2, length 4
echo ${#a[@]} ${#s}
[[ "${a[@]}" = "$s" ]] && echo EQUAL
