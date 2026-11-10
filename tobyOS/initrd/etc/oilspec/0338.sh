a=( '12 3' )
b=( "${a[@]}" )
c="${a[@]}"  # This decays it to a string
d=${a[*]}  # This decays it to a string
echo ${#a[0]} ${#b[0]}
echo ${#a[@]} ${#b[@]}

# osh is intentionally stricter, and these fail.
echo ${#c[0]} ${#d[0]}
echo ${#c[@]} ${#d[@]}
