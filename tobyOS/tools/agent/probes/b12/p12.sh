a=(10 20 30 40); i=1; j='i+1'
echo "1 $(( a[1] )) $(( a[i] )) $(( a[$i] )) $(( a["$i"] )) $(( a[j] )) $(( a[$j] ))"
echo "2 $(( a[a[0]/10] )) $(( a[ 2 ] + a[-1] ))"
(( a[2] += 5 )); echo "3 ${a[@]}"
for ((k = 0; k < ${#a[@]}; k++)); do s=$((s + a[k])); done; echo "4 $s"
for ((k = 0; a[k] < 25; k++)); do :; done; echo "5 $k"
echo "6 $[ a[1] * 2 ]"
declare -A A; A[x]=3; A['a b']=4; A['$x']=5; A['\']=6; A[']']=7
k='a b'; v='$x'; b='\'; r=']'
echo "7 $(( A[x] )) $(( A[$k] )) $(( A["$k"] )) $(( A[$v] )) $(( A[$b] )) $(( A["$r"] ))"
echo "8 $(( A['a b'] + A["x"] ))"
(( A[$k]++ )); (( A["new key"] = 9 )); echo "9 ${A['a b']} ${A['new key']}"
c='$(echo HI >&2; echo 1)'; echo "10 $(( a[$c] ))" 2>&1
echo "11 $(( a[$(echo 3)] ))"
long=$(printf '%0200d' 7); A[$long]=L; echo "12 ${#A[$long]} ${A[$long]}"
printf -v "A[$long]" '%s' M; echo "13 ${A[$long]}"
printf -v 'A[p\ q]' '%s' N; echo "14 ${A['p q']}" "$(declare -p A | od -c | grep -c 004)"
read 'A[r\ s]' <<< R; echo "15 ${A['r s']}"
declare -n nr="A[$long]"; echo "16 $nr"
unset "A[$long]"; echo "17 [${A[$long]}] ${#A[@]}"
x=5; echo "18 $(( x[0] )) $(( "x" + 1 )) $(( 'x' ))" 2>&1
echo "19 $(( a[1]+a[2] )) $(( a[1] ? a[2] : a[3] ))"
set -- 1 2; echo "20 $(( a[$#] )) $(( a[$@] ))" 2>&1
m=(); (( m[3] = 7, m[1] = 2 )); echo "21 ${!m[@]} ${m[@]}"
e=; for ((i=0; ""; i++)); do echo never; break; done; echo "22 done"
echo "23 $(( 1 + [2] ))" 2>&1
echo "24 $(( a[] ))" 2>&1
echo "25 $(( A[~] ))" 2>&1; A['~']=8; echo "26 $(( A[~] ))"
A['#h']=9; echo "27 $(( A[#h] ))"
z=(1 2 3); echo "28 ${z[@]: -1}" "${z[@]:1:1}"
declare -A S=([a]=1 [b]=2 [c]=3); echo "29 ${#S[@]}" "$(( ${#S[@]} + 1 ))"
