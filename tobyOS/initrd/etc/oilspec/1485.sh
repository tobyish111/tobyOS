shopt -u strict_arith || true
K=key
V=value
typeset -a a
(( a[K] = V ))

# not there!
echo a[\"key\"]=${a[$K]}

echo keys = ${!a[@]}
echo values = ${a[@]}
