# An array literal assigned to an integer variable evaluates each element,
# its attribute read before the scalar it replaces goes (bash's
# appendop.tests: `typeset -i x; x=([0]=7+11)` is 18).
unset x; typeset -i x; x=([0]=7+11); echo ${x[@]}
unset y; declare -i y; y=(1+1 2*3 [5]=10/2); echo "${y[@]}"
unset u; declare -u u; u=(ab cd); echo "${u[@]}"
