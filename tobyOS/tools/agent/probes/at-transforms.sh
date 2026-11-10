a=(x "y z"); declare -A A=([k]=v [j]="w x"); x=x; e=""
echo "${a[@]@K}"; echo "${A[@]@K}"
printf "<%s>" "${a[@]@k}"; echo; printf "<%s>" "${A[@]@k}"; echo
echo "${x@K}" "${e@K}" "${u@K}" "${x@Q}" "${u@Q}"; echo "${a@K}" "${a[1]@K}"
echo "${a[*]@K}"; printf "<%s>" ${a[@]@K}; echo
echo "${a[0]@a}" "${a@a}" "${A[k]@a}"; printf "<%s>" "${a[@]@a}"; echo; printf "<%s>" "${u[@]@a}" "${u[*]@a}"; echo
declare -i n=1; declare -l lo=AB; declare -u up=ab; echo "${n@a}" "${lo@a}" "${up@a}"
r=a; echo "${!r@a}"; r="a[0]"; echo "${!r@a}"
