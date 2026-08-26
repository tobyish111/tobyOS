K=5
V=42
typeset -A A
(( A[K] = V ))

echo A["5"]=${A["5"]}
echo keys = ${!A[@]}
echo values = ${A[@]}
