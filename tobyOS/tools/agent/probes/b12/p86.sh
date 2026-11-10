exec 2>&1
qb=1; qa=1; qc=(1); declare -a qd; qe=2
declare -p | grep -E '^declare -[-a] q[a-e]'
set | grep -E '^q[a-e]='
declare -a | grep -E ' q[a-e]'
