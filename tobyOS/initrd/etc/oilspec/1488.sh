shopt -u strict_arith || true
K=key
V=value
typeset -A A || exit 1
(( A["K"] = V ))

# not there!
echo A[\"key\"]=${A[$K]}

echo keys = ${!A[@]}
echo values = ${A[@]}
