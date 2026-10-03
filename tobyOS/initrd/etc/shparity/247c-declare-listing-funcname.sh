# FUNCNAME outside a function is declared and invisible (`declare -p
# FUNCNAME` says `declare -a FUNCNAME`, `set` and -v do not see it), and
# `unset FUNCNAME` takes it away for good; the `declare -p`/`declare -a`
# listing is sorted by name, a declared-empty array included; the dynamic
# variables print as bash's do. tsh said "not found" and listed in
# creation order.
# (batch 12; probe p86 in C:\t\b12)
exec 2>&1
qb=1; qa=1; qc=(1); declare -a qd; qe=2
declare -p | grep -E '^declare -[-a] q[a-e]'
set | grep -E '^q[a-e]='
declare -a | grep -E ' q[a-e]'
