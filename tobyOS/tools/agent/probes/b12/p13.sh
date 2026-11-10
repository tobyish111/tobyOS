exec 2>&1
a=(1 2 3); declare -A A=([x]=1)
echo "1 $(( a[] ))"; echo "s=$?"
echo "2 $(( a[] + 5 ))"; echo "s=$?"
(( a[] = 4 )); echo "3 s=$? ${a[@]}"
echo "4 $(( A[] ))"; echo "s=$?"
(( A[] = 4 )); echo "5 s=$? ${!A[@]}"
echo "6 $(( u[] ))"; echo "s=$?"
x=$(( a[] )); echo "7 s=$? [$x]"
echo "8 $(( a[ ] ))"; echo "s=$?"
echo "9 $(( A[ ] ))"; echo "s=$?"
