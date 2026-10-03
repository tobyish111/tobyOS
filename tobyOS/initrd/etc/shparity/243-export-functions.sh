# export -f puts BASH_FUNC_NAME%%=() { ... } in a child's environment, as
# bash does, and a shell imports one at start -- refusing anything more than
# one definition. declare -F shows -fx; export -nf takes it back. tsh had
# no exported functions. (Variable order in env is not bash's hash order;
# each line is looked at on its own; a nested shell's own name is
# replaced by SH, as the runner does for stderr.)
# (batch 12; probe p77 in C:\t\b12)
exec 2>&1
f() { echo "hi $1"; for i in 1; do echo "$i"; done; }
g() ( echo sub )
export -f f g
env | grep '^BASH_FUNC_f%%' | od -c | head -6
env | grep -c '^BASH_FUNC_g%%'
$BASH -c 'f x; g; declare -F; declare -p BASH_FUNC_f%%' 2>&1 | sed 's#^[^ ]*: #SH: #'
export -f
declare -F
export -nf g
declare -F
env | grep -c BASH_FUNC
h() { :; }; declare -fx h; declare -F | grep ' h$'
export -f nosuch; echo "s=$?"
x=1; export -f x; echo "s=$?"
env 'BASH_FUNC_k%%=() { echo imported; }' $BASH -c 'k; declare -F k; echo "${BASH_FUNC_k%%-unset}"' 2>&1 | sed 's#^[^ ]*: #SH: #'
env 'BASH_FUNC_bad%%=() { echo a; }; echo injected' $BASH -c 'bad; echo after' 2>&1 | sed 's#^[^ ]*: #SH: #'
