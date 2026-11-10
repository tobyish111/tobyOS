exec 2>&1
f() { echo "hi $1"; for i in 1; do echo "$i"; done; }
g() ( echo sub )
export -f f g
env | grep BASH_FUNC | od -c | head -12
$BASH -c 'f x; g; declare -F; declare -p BASH_FUNC_f%% 2>&1 | head -1'
export -f
declare -F
export -nf g
declare -F
env | grep -c BASH_FUNC
h() { :; }; declare -fx h; declare -F | grep ' h$'
export -f nosuch; echo "s=$?"
x=1; export -f x; echo "s=$?"
env 'BASH_FUNC_k%%=() { echo imported; }' $BASH -c 'k; declare -F k; echo "${BASH_FUNC_k%%-unset}"'
env 'BASH_FUNC_bad%%=() { echo a; }; echo injected' $BASH -c 'bad; echo after'
