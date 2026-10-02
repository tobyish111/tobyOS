x='a b'; s=1; s+=$x; echo "[$s]"
y='c d'; t=$y; echo "[$t]"
a=(1); a+=$x; declare -p a
a[0]+=$x; declare -p a
