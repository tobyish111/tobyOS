# A substring offset or length that names an element by a variable key
# has the subscript expanded once and quoted, as (( )) does: with k3=']',
# ${s:A[%]:A[$k3]} reads A's `]` element -- tsh expanded the text whole
# and the evaluator met `A[]]` (finding 484).
exec 2>&1
declare -A A
s=abcdefghijklmnopqrstuvwxyz
k1='%'; k2='$(echo %)'; k3=']'
A[%]=10; A[']']=10; A[$k2]=5
echo ${s:A[%]:A[$k1]}
echo ${s:A[%]:A[$k2]}
echo ${s:A[%]:A[$k3]}
declare -a ar=(1 2 3)
i='$(echo 0)'
echo "${s:ar[1]:ar[2]}"
