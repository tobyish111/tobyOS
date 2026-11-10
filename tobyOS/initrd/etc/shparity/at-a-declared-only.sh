# @a of a name declared and never given a value is its attributes through a
# subscript too -- declare -rl V; ${V[@]@a} and ${V[0]@a} are rl, declare -A
# m; ${m[@]@a} is A -- where tsh gave nothing, or only the l (finding 409).
exec 2>&1
declare -rl V1; echo "[${V1[@]@a}]" "[${V1[*]@a}]" "[${V1[0]@a}]" "[${V1@a}]"
declare -arl V3; echo "[${V3[@]@a}]" "[${V3[0]@a}]"
declare -A m; echo "[${m[@]@a}]"; declare -A m2=(); echo "[${m2[@]@a}]"
declare -a e=(); echo "[${e[@]@a}]"; declare -i n; echo "[${n[@]@a}]" "[${n[1]@a}]"
declare -rl s=x; echo "[${s[0]@a}]"
a=(1 2); declare -r a; echo "[${a[@]@a}]"
