declare -a a b
a=(x y z)
b="${a[@]}"  # this collapses to a string
c=("${a[@]}")  # this preserves the array
c[1]=YYY  # mutate a copy -- doesn't affect the original
argv.py "${a[@]}"
argv.py "${b}"
argv.py "${c[@]}"
