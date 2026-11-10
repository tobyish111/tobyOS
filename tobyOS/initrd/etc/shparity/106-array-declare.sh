# declare -a / local -a create arrays; declare -p reproduces them.
declare -a d
d[1]=one
echo "d n=${#d[@]} 1=${d[1]}"
declare -a e=(x y z)
echo "e n=${#e[@]} all=${e[*]}"
f() { local -a g=(p q); echo "local n=${#g[@]} 1=${g[1]}"; }
f
echo "outside=[${g[*]}]"
declare -a h=()
echo "empty n=${#h[@]}"
echo end
