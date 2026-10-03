# BASH_SUBSHELL in ( ), $( ), pipelines and & ; exporting long names and
# unsetting an exported associative array.
# (batch 12; probe p41 in C:\t\b12)
exec 2>&1
declare -A q; q[k]=v; unset 'q[k]'; echo "1 ${#q[@]}"; unset q
export q=e; env | grep -c '^q=e'; declare -p q
declare -a r=(1); unset r; export r=e; env | grep -c '^r=e'; declare -p r
s=(1 2); export s; env | grep -c '^s='; declare -p s
