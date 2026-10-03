exec 2>&1
x='a b'
a=(1); a[0]+=$x; declare -p a
a=(1); a[0]=$x; declare -p a
a=(1); a[0]+="$x"; declare -p a
s=1; s+=$x; echo "[$s]"
a=(1)
a[0]+=$x
declare -p a
b=(1); b[1]=$x; declare -p b
