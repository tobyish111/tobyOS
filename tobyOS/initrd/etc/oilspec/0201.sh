declare -A A
(( A[5] = 10 ))
(( A[5] += 6 ))
echo ${A[5]}
