# declare -x and local -x of an array literal keep the attribute, and
# local -a NAME=() is an empty array, not a name only declared (finding
# 309): an array has no entry of its own, and its flags went nowhere.
declare -rx r=(1); declare -p r; declare -x a=(1 2); declare -p a
declare -xA A=([k]=v); declare -p A; declare -x -A M=(); declare -p M
echo "${a@a} ${a[@]@A}"; declare +x a; declare -p a
env | grep -c '^a='
f() {
    local -x l=(9); declare -p l
    local -xa m=(); declare -p m
    local -a e=(); declare -p e
    local -ai k=(); declare -p k
    local -a y=(); y+=(1); declare -p y
    local -a z; echo "<${z[@]}> ${#z[@]}"; declare -p z
}
f
