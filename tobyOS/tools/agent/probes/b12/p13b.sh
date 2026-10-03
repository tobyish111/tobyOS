exec 2>&1
a=(1 2 3)
echo "1 $(( a[] = 4 ))"; echo "s=$?"
x=$(( a[] = 4 )); echo "2 s=$? [$x]"
let 'a[]=4'; echo "3 s=$?"
let 'y=a[]'; echo "4 s=$? y=$y"
(( a[]++ )); echo "5 s=$?"
echo "6 $(( a[]++ ))"
declare -i n; n='a[]'; echo "7 s=$? n=$n"
f() { echo "8 $(( a[] ))"; }; f
echo "9 ${a[]}"; echo "s=$?"
echo "10 $(( a[-9] ))"; echo "s=$?"
(( a[-9] = 1 )); echo "11 s=$?"
