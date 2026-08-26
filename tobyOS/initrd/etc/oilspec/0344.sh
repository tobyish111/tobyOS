shopt -u strict_arith
a[a]=42
a[a]=99
argv.py "${a[@]}" "${a[0]}" "${a[42]}" "${a[99]}"
