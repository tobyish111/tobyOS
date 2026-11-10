# An associative array iterates in bash's hash-table order (FNV-1 over the
# key, 1024 buckets, newest first within a bucket) -- ${!A[@]}, ${A[@]},
# declare -p, for. tsh listed keys in insertion order.
exec 2>&1
declare -A A=([a]=1 [b]=2 [c]=3); declare -p A; echo "${!A[@]}"
declare -A B
for k in one two three four five six seven eight nine ten; do B[$k]=$k; done
echo "${!B[@]}"
unset 'B[three]'; B[eleven]=11; echo "${B[@]}"
for k in "${!B[@]}"; do printf '%s ' "$k"; done; echo
