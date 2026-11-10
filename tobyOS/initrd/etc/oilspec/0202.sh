shopt -u strict_arith  # default zero cell
declare -A A
(( A[5] += 6 ))
echo ${A[5]}
