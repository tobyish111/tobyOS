# declare -r or -x of an array assigned an empty list keeps it assigned:
# `declare -a o=(); declare -r o` is `declare -ar o=()`, as readonly's is
# -- for an associative one too; one only declared stays `declare -A` with
# no list (finding 380). tsh printed the assigned array as only declared.
exec 2>&1
declare -a o=(); declare -r o; declare -p o
declare -a q=(); declare -x q; declare -p q
declare -A r=(); declare -r r; declare -p r
s=(); declare -r s; declare -p s
declare -A fluff; declare -x fluff; declare -p fluff
