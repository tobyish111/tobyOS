# Element operations: negative-index appends, long names, subscripts
# expanded after the value, scalar-as-array slices, "$@" and arrays inside
# ${u:-...} words, assigning a scalar to an array.
# (batch 12; probe p26 in C:\t\b12)
exec 2>&1
n=$(printf 'v%.0s' {1..200})
eval "$n=1"; eval "echo 1 \${#$n} \$$n"
eval "$n+=2"; eval "echo 2 \$$n"
eval "$n[3]=x"; eval "echo 3 \${$n[3]} \${#$n[@]}"
eval "$n+=(y z)"; eval "echo 4 \${$n[@]}"
eval "declare -n r=$n"; echo "5 ${r[@]}"; r=Q; eval "echo 5b \${$n[@]}"
eval "unset $n"; eval "echo 6 [\${$n-unset}]"
eval "declare -A $n"; eval "$n[k]=v"; eval "echo 7 \${$n[k]}"
eval "unset '$n[k]'"; eval "echo 8 \${#$n[@]}"; eval "unset $n"
eval "export $n=e"; env | grep -c "^$n=e"
eval "readonly $n"; eval "$n=w" ; echo "9 s=$?"
f() { eval "local $n=L"; eval "echo 10 \$$n"; }; f
eval "printf -v $n %s P" 2>&1 | head -1
read -r $n <<< R 2>&1; eval "echo 11 \$$n"
m=$(printf 'w%.0s' {1..150}); eval "$m=(a b c)"; eval "echo 12 \${!$m[@]} \${$m[1]}"
eval "echo 13 \${!${m:0:3}@}" | wc -c
declare -n rr="$m[1]"; echo "14 $rr"; rr=B; eval "echo 15 \${$m[@]}"
eval "(( $m[2] = 7 ))"; eval "echo 16 \${$m[2]}"
eval "test -v '$m[2]'"; echo "17 $?"
eval "$m[1]+=Z"; eval "echo 18 \${$m[1]}"
eval "declare $m[5]=d"; eval "echo 19 \${$m[5]}"
k=$(printf 'k%.0s' {1..300}); declare -A AA; AA[$k]=1; echo "20 ${#AA[@]} ${AA[$k]}"
printf -v "AA[$k]" %s 2; echo "21 ${AA[$k]}"
unset "AA[$k]"; echo "22 ${#AA[@]}"
