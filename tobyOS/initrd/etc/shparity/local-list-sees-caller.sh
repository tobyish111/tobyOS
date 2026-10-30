# A new local's list is expanded before the local is made, so it sees the
# caller's variable of the same name: `local -a arr=( "${arr[@]}" )` copies
# it -- for local, declare and typeset, with -i and -A (finding 378). tsh
# made the local first and copied the empty local.
exec 2>&1
arr=(zero one two); f() { local -a arr=( "${arr[@]}" ); arr+=(x); declare -p arr; }; f; declare -p arr
g() { declare -a arr=( "${arr[@]}" two ); declare -p arr; }; g
h() { typeset -i n=(1+1 2*3); declare -p n; }; h
k() { local -A m=([k]="it's" [x y]=z); declare -p m; }; k
set -u; name='arr[@]'; f2() { local -a arr=("${!name}"); declare -p arr; }; f2
