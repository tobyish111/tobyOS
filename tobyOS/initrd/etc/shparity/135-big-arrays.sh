a=(); i=0; while [ $i -lt 3000 ]; do a[i]=$i; i=$((i+1)); done
echo "a1 [${#a[@]}] [${a[511]}] [${a[512]}] [${a[2999]}]"
b=($(i=0; while [ $i -lt 2500 ]; do echo $i; i=$((i+1)); done)); echo "a2 [${#b[@]}] [${b[2499]}]"
c=("${a[@]}" "${b[@]}"); echo "a3 [${#c[@]}]"
a+=(x y z); echo "a4 [${#a[@]}] [${a[-1]}]"
unset 'a[1000]'; echo "a5 [${#a[@]}]"
declare -A m; i=0; while [ $i -lt 1000 ]; do m[k$i]=$i; i=$((i+1)); done; echo "a6 [${#m[@]}] [${m[k999]}]"
s=0; for v in "${a[@]:2990:5}"; do s=$((s+v)); done; echo "a7 [$s]"
i=0; while [ $i -lt 150 ]; do eval "arr_$i=(x$i y$i)"; i=$((i+1)); done
echo "a8 [${arr_0[1]}] [${arr_63[0]}] [${arr_64[0]}] [${arr_149[1]}]"
echo done
