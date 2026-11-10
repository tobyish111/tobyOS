# unset of a function's own local leaves it a local, declared with no value
# and no attribute -- `declare -- v`, and a later v=2 is still the local's;
# one made over a prefix assignment's binding keeps its -x (finding 377).
# tsh removed the local: "declare: v: not found".
exec 2>&1
f() { local v=x; unset v; declare -p v; echo "[${v-unset}]"; }; v=g; f; echo "[$v]"
f() { local v=x; unset v; declare -p v; }; v=t f; echo "[$v]"
f() { local -i n=1; unset n; declare -p n; n=2; declare -p n; }; f
f() { local -a a=(1); unset a; declare -p a; }; f
