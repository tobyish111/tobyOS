a=(1 2 3); b=(4 5); e=(); s=("x y" "" z)
c=("${a[@]}"); declare -p c
c=("${a[@]}" "${b[@]}"); declare -p c
c=(x "${a[@]}" y); declare -p c
c=(${a[@]}); declare -p c
c=("${e[@]}" z); declare -p c
c=("${e[@]}"); declare -p c
c=(pre"${a[@]}"post); declare -p c
c=("${s[@]}"); declare -p c
c=(${s[@]}); declare -p c
c=("${a[*]}"); declare -p c
set -- p "q r"; c=("$@"); declare -p c
set --; c=("$@" k); declare -p c
c+=("${b[@]}"); declare -p c
echo done
