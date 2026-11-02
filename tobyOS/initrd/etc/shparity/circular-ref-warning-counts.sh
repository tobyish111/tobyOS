# A function's local reference to itself warns "circular name reference"
# once for each time bash looks the name up, and how many that is depends
# on the use: a pattern or case operator, an element, an arithmetic or
# element assignment, unset, -v and declare -r each look more than once.
# A write through a chain back via a global reference warns not at all,
# ${!v} of the reference is an invalid indirect expansion, and readonly
# makes the global readonly. tsh warned once a use, turned ${!v} round,
# and left the global writable (finding 573).
exec 2>&1
f() {
    typeset -n v=v
    : ${v} ${v#g} ${v^^}
    : ${v[1]} ${v[@]:1}
    ((v+=1)); v[1]=q
    [[ -v v ]]; test -v v
}
v=gv; f; echo "rc=$? [$v]"
g() { typeset -n v=v; unset v[0]; declare -x v; }; v=(a b); g; declare -p v
h() { typeset -n v=v; echo "[${!v}]"; }; v=gv; h
echo "rc=$?"
declare -n w=v; k() { local -n v=w; v=X; echo "[$v]"; }; v=gv; k
unset -n w
r() { typeset -n v=v; readonly v; }; v=gv; r; v=new; echo "rc=$? [$v]"
