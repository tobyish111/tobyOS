# Element operations: negative-index appends, long names, subscripts
# expanded after the value, scalar-as-array slices, "$@" and arrays inside
# ${u:-...} words, assigning a scalar to an array.
# (batch 12; probe p25 in C:\t\b12)
exec 2>&1
a=(1 '2 3')
a[-1]+=(4 5); echo "1 s=$? ${a[@]}"
s='abc'; echo "2 ${s[@]} ${s[*]}"
default=('1 2' '3'); printf '<%s>' "${undef[@]:-${default[@]}}"; echo " 3"
printf '<%s>' ${undef[@]:-${default[@]}}; echo " 3b"
printf '<%s>' "${undef[*]:-${default[@]}}"; echo " 3c"
printf '<%s>' "${default[@]:+${default[@]}}"; echo " 3d"
a=( '12 3' ); b=( "${a[@]}" ); c="${a[@]}"; d=${a[*]}
echo "4 ${#a[0]} ${#b[0]} ${#a[@]} ${#b[@]} ${#c[0]} ${#d[0]} ${#c[@]} ${#d[@]}"
a=(1); unset -v 'a[-2]'; echo "5 s=$?"; unset -v "a[-3]"; echo "5b s=$?"
a=(0 1 2); b=(3 4 5); HOME=/home/spec-test
a[0 + 1]=  b[2 + 0]=~/src
typeset -p a b
a[0 + 1]='foo' printf '<%s>' b[2 + 0]='bar'; echo " 6 s=$?"
typeset -p a b
a=(); a[5 + 1]=; a[5 / 1]=y; echo "7 len=${#a[@]}"
a=(); a["1"]=2; echo "8 status=$? len=${#a[@]}"
a['2']=3; echo "8 status=$? len=${#a[@]}"
a[2 + "3"]=5; echo "8 status=$? len=${#a[@]}"
a[3 + '4']=5; echo "8 status=$? len=${#a[@]}"
a=(4 5 6); (( b = a )); echo "9 ${a[@]} / ${b[@]}"
declare +a 'xyz1=1'; declare +a 'xyz2=(2 3)'; declare -a 'xyz3=4'; declare -a 'xyz4=(5 6)'
printf '<%s>' "${xyz1}" "${xyz2}" "${xyz3[@]}" "${xyz4[@]}"; echo " 10"
declare -p xyz1 xyz2 xyz3 xyz4
a[5 # 1]=
echo "11 s=$?"
