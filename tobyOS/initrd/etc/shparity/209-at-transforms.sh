# ${x@a} reports i, l and u too; ${a[0]@a} and ${a[@]@a} are the ARRAY's
# attributes; ${a[@]@K} is KEY "VALUE" pairs, ${a[@]@k} the same as words;
# ${x@Q} of an unset x is nothing. tsh: '' per element, bad substitution for
# K and k, and '' for an unset @Q.
exec 2>&1
a=(x "y z"); declare -A A=([k]=v [j]="w x"); x=x; e=""
echo "${a[@]@K}"; echo "${A[@]@K}"
printf "<%s>" "${a[@]@k}"; echo; printf "<%s>" "${A[@]@k}"; echo
echo "${x@K}" "${e@K}" "${u@K}" "${x@Q}" "${u@Q}"; echo "${a@K}" "${a[1]@K}"
echo "${a[*]@K}"; printf "<%s>" ${a[@]@K}; echo
echo "${a[0]@a}" "${a@a}" "${A[k]@a}"; printf "<%s>" "${a[@]@a}"; echo; printf "<%s>" "${u[@]@a}" "${u[*]@a}"; echo
declare -i n=1; declare -l lo=AB; declare -u up=ab; echo "${n@a}" "${lo@a}" "${up@a}"
r=a; echo "${!r@a}"; r="a[0]"; echo "${!r@a}"
