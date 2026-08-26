K=5
V=42
typeset -a array
(( array[K] = V ))

echo array[5]=${array[5]}
echo keys = ${!array[@]}
echo values = ${array[@]}
