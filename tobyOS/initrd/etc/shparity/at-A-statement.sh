# ${NAME@A} is bash's assignment statement for NAME in every form (finding
# 308): `declare -FLAGS` for a name with attributes, the attributes alone
# for one with no value, an element's value under its array's statement,
# the array's declare -p line for ${a[@]@A}, `set --` and the quoted
# parameters for ${@@A}; nothing for a special parameter. tsh printed its
# scratch names (SHELL_ELEM_0, SHELL_ARRAY_ELEM), and ${@@K} ${@@k} ${@@a}
# as an array's.
declare -i x=1; export y=2; declare -l w=Ab; declare -i n; declare -a d
printf '<%s>' "${x@A}" "${y@A}" "${w@A}" "${n@A}" "${d@A}" "${u@A}"; echo
declare -ai ai=(1 2); declare -A A=([k]=v); a=(x 'y z')
printf '<%s>' "${ai[1]@A}" "${A@A}" "${A[k]@A}" "${a[5]@A}" "${a[-1]@A}"; echo
printf '<%s>' "${a[@]@A}"; echo; printf '<%s>' "${a[*]@A}"; echo
printf '<%s>' ${a[@]@A}; echo; printf '<%s>' "${ai[@]@A}" "${A[*]@A}"; echo
set -- ab 'c d'
printf '<%s>' "${@@A}"; echo; printf '<%s>' "${*@A}"; echo; printf '<%s>' ${@@A}; echo
printf '<%s>' "${@@K}"; echo; printf '<%s>' "${*@k}"; echo; printf '<%s>' "${@@a}" "${*@a}"; echo
printf '<%s>' "${1@A}" "${#@A}"; echo
IFS=:; printf '<%s>' "${@@A}" "${*@A}" "${a[@]@A}"; echo; unset IFS
f() { printf '<%s>' "${@@A}"; echo; }; f 1 $'a\tb' "it's"
set --; n() { echo $#; }; n "${@@A}"; n "${*@A}"; e=(); n "${e[@]@A}"
eval "${a[@]@A}"; b=("${a[@]}"); eval "$(set -- p q; echo "${@@A}"); echo \$2"
