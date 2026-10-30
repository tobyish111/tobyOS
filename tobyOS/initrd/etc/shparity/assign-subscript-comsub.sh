# An assignment word's subscript may hold a command substitution with a
# `]` in it, or a ${...}: `K[$(echo ])]=def` is an assignment (finding 365).
# tsh ran it as a command, "command not found".
exec 2>&1
declare -A K; K[$(echo ])]=def; echo "rc=$?"; declare -p K
a[$(echo ] >/dev/null; echo 2)]=two; a[${x:-3}]=three; declare -p a
