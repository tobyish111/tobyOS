# Element operations: negative-index appends, long names, subscripts
# expanded after the value, scalar-as-array slices, "$@" and arrays inside
# ${u:-...} words, assigning a scalar to an array.
# (batch 12; probe p32 in C:\t\b12)
exec 2>&1
i=0; a=(); a[i++]=$i; echo "1 i=$i $(declare -p a)"
i=0; a=(); a[$i]=$((i+=5)); echo "2 i=$i $(declare -p a)"
i=0; a=(); a[$((i+=2))]=$((i+=10)); echo "3 i=$i $(declare -p a)"
declare -A A; k=K; A[$k]=$((k=1)); echo "4 $(declare -p A)"
a=(); a["1"]=x; a[2+"3"]=y; echo "5 $(declare -p a)"
a=(); a['2']=z; echo "6 $(declare -p a)"
echo "6b s=$?"
A=(); A['q']=1; A["r s"]=2; A[$'t\tu']=3; echo "7 $(declare -p A)"
a=(); v='1+1'; a[$v]=p; a["$v"]=q; echo "8 $(declare -p a)"
a=(); a[\$x]=r; echo "9 s=$?"
x=4; a=(); a[$x]=s a[x+1]=t; echo "10 $(declare -p a)"
declare a[$x+2]=u; echo "11 $(declare -p a)"
declare -A B; B[a b]=1; B[ c ]=2; echo "12 $(declare -p B)"
a=(); a[ 1 ]=w; echo "13 $(declare -p a)"
s=str; echo "14 ${s[@]} ${#s[@]} ${s[*]} ${!s[@]} ${s[0]} ${s[1]-u}"
f=(); echo "15 ${#f[@]} [${f[@]}]"
