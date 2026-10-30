# wait -p takes an element as its name, as read does: `wait -p A[$k]`, the
# key read's would be (finding 374). tsh refused it, "not a valid
# identifier".
exec 2>&1
declare -A A; k=']'; shopt -s assoc_expand_once; { sleep 0.1; exit 4; } & p=$!; wait -p A[$k] -n; echo "rc=$? $([ "${A[$k]}" = "$p" ] && echo same)"
declare -A B; { sleep 0.1; exit 3; } & p=$!; wait -p B[x] -n; echo "rc=$? $([ "${B[x]}" = "$p" ] && echo same)"
c=(); { sleep 0.1; exit 2; } & p=$!; wait -p c[3] -n $p; echo "rc=$? $([ "${c[3]}" = "$p" ] && echo same)"
