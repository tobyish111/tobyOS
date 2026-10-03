# Namerefs: ${!ref}, element targets, the checks declare -n and local -n
# make -- a self reference is an error at top level and a warning in a
# function, an empty value still makes the local.
# (batch 12; probe p29 in C:\t\b12)
exec 2>&1
a=('A B' C); typeset -n ra='a[@]'
ra=v; echo "1 s=$? ${a[@]}"
ra=(X Y Z); echo "2 s=$? ${a[@]}"
ra+=x; echo "3 s=$? ${a[@]}"
echo "4 ${#ra} ${ra:1} [${ra/A/Q}] ${ra-u} ${!ra}"
printf '<%s>' "${ra}" "${ra[0]}" "${ra[@]}" $ra; echo " 5"
array=(X Y Z); typeset -n rz='array[0]'
rz=(1 2); echo "6 s=$? ${array[@]}"
rz[0]=foo; echo "7 s=$? ${array[@]}"
rz[1]=x; echo "8 s=$? ${array[@]}"
rz=new; echo "9 s=$? ${array[@]}"
rz+=more; echo "10 s=$? ${array[@]}"
echo "11 ${rz} ${#rz} ${rz[0]} [${rz[@]}]"
typeset -n rs='a[*]'; IFS=:; echo "12 $rs ${rs}"; printf '<%s>' "$rs"; echo; IFS=' '
unset ra; echo "13 s=$? ${#a[@]}"
